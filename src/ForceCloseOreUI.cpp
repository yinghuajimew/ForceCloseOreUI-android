#include <sys/stat.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <unistd.h>
#include <cstdarg>
#include <android/log.h>
#include <errno.h>
#include <dobby.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;

static std::string packageName;
static std::string getPackageName();
static std::string getConfigDir();
static std::string g_logFilePath;

// ------------------------------------------------------------
// 双通道日志（文件 + Logcat）
// ------------------------------------------------------------
static void WriteLog(const char* level, const char* format, ...) {
    va_list args;
    va_start(args, format);
    char buf[4096];
    vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    
    int prio = (level[0] == 'E') ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO;
    __android_log_print(prio, "ForceCloseOreUI", "[%s] %s", level, buf);
    
    if (!g_logFilePath.empty()) {
        try {
            FILE* file = fopen(g_logFilePath.c_str(), "a");
            if (file) {
                fprintf(file, "[%s] %s\n", level, buf);
                fclose(file);
            }
        } catch (...) {}
    }
}

#define LOGI(...) WriteLog("INFO", __VA_ARGS__)
#define LOGE(...) WriteLog("ERROR", __VA_ARGS__)

// ------------------------------------------------------------
// Game classes
// ------------------------------------------------------------
class OreUIConfig {
public:
    void *mUnknown1;
    void *mUnknown2;
    std::function<bool()> mUnknown3;
    std::function<bool()> mUnknown4;
};

class OreUi {
public:
    std::unordered_map<std::string, OreUIConfig> mConfigs;
};

// ------------------------------------------------------------
// 分离版的 Signatures
// ------------------------------------------------------------

// 1. 新版本 (11 参数 V10) 专属特征码
static const std::vector<const char*> SIG_V10 = {
    // [1] sub_AFB75A0 | 得分: 155
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 5A D0 3B D5 F4 03 07 AA F6 03 06 AA 48 17 40 F9 F8 03 05 AA F9 03 04 AA F5 03 02 AA F7 03 01 AA",
    // [2] sub_804F118 | 得分: 150
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 5C D0 3B D5 F3 03 00 AA E0 03 02 AA 88 17 40 F9 F8 03 07 AA FA 03 06 AA F9 03 05 AA",
    // [3] sub_A7F5F3C | 得分: 150
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 5B D0 3B D5 F6 03 07 AA F7 03 06 AA 68 17 40 F9 F9 03 05 2A FA 03 02 AA F5 03 04 AA",
};

// 2. 旧版本 (6 参数 V1) 兜底特征码
static const std::vector<const char*> SIG_V1 = {
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F7 03 05 AA FB 03 03 2A",
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FB 03 03 2A F8 03 02 2A",
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA",
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? F9 ? ? ? D5 FB 03 00 AA ? ? ? F9 F5 03 07 AA",
    "? ? ? D1 ? ? ? A9 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA",
    "? ? ? D1 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA",
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA",
};

// ------------------------------------------------------------
// 自验证标志（Hook 回调写入，tryHookGroup 读取）
// ------------------------------------------------------------
static volatile bool g_hookValid = false;

// ------------------------------------------------------------
// Hook function pointers
// ------------------------------------------------------------
typedef void (*OreUiInitFuncV1)(OreUi&, void*, void*, void*, void*, void*);
static OreUiInitFuncV1 orig_v1 = nullptr;

typedef void (*OreUiInitFuncV10)(void*, void*, void*, void*, void*, void*, void*, void*, void*, OreUi&, void*);
static OreUiInitFuncV10 orig_v10 = nullptr;

// ------------------------------------------------------------
// Configuration Functions
// ------------------------------------------------------------
static std::string getPackageName() {
    std::ifstream cmdline("/proc/self/cmdline");
    std::string pkg;
    std::getline(cmdline, pkg, '\0');
    return pkg.empty() ? "com.mojang.minecraftpe" : pkg;
}

static std::string getConfigDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    
    const std::string& pkg = packageName.empty() ? getPackageName() : packageName;
    
    std::vector<std::string> candidates = {
        "/storage/emulated/0/games/com.mojang/ForceCloseOreUI/",
        "/storage/emulated/0/Android/data/" + pkg + "/mods/ForceCloseOreUI/",
    };
    
    for (const auto& base : candidates) {
        size_t lastSlash = base.find_last_of('/', base.size() - 2);
        std::string parent1 = base.substr(0, lastSlash);
        lastSlash = parent1.find_last_of('/');
        std::string parent2 = parent1.substr(0, lastSlash);
        
        mkdir(parent2.c_str(), 0755);
        mkdir(parent1.c_str(), 0755);
        mkdir(base.c_str(), 0755);
        
        std::string testFile = base + ".test";
        FILE* fp = fopen(testFile.c_str(), "wb");
        if (fp) {
            fclose(fp);
            unlink(testFile.c_str());
            cached = base;
            g_logFilePath = base + "debug.log";
            LOGI("Using config dir: %s", base.c_str());
            return cached;
        }
        LOGI("Path not writable: %s (errno=%d: %s)", base.c_str(), errno, strerror(errno));
    }
    
    std::string fallback = "/data/data/" + pkg + "/files/ForceCloseOreUI/";
    mkdir(("/data/data/" + pkg + "/files").c_str(), 0755);
    mkdir(fallback.c_str(), 0755);
    cached = fallback;
    g_logFilePath = fallback + "debug.log";
    LOGE("All external paths failed! Using internal: %s", fallback.c_str());
    return cached;
}

static std::string normPath(const fs::path& p) { return p.lexically_normal().generic_string(); }

static std::string trimAscii(std::string value) {
    auto is_space = [](unsigned char c) { return std::isspace(c) != 0; };
    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), is_space));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), is_space).base(), value.end());
    return value;
}

static std::optional<bool> parseBoolString(std::string value) {
    value = trimAscii(std::move(value));
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "true" || value == "1" || value == "yes" || value == "on" || value == "enabled") return true;
    if (value == "false" || value == "0" || value == "no" || value == "off" || value == "disabled") return false;
    return std::nullopt;
}

static std::optional<bool> readBoolLike(const Json& value) {
    try {
        if (value.is_boolean())         return value.get<bool>();
        if (value.is_number_integer())  return value.get<long long>() != 0;
        if (value.is_number_unsigned()) return value.get<unsigned long long>() != 0;
        if (value.is_number_float())    return value.get<double>() != 0.0;
        if (value.is_string())          return parseBoolString(value.get<std::string>());
        if (value.is_object()) {
            for (std::string_view key : {"value", "enabled", "enable", "state", "default"}) {
                auto it = value.find(std::string(key));
                if (it != value.end()) { if (auto parsed = readBoolLike(*it)) return parsed; }
            }
            auto disabled = value.find("disabled");
            if (disabled != value.end()) { if (auto parsed = readBoolLike(*disabled)) return !*parsed; }
        }
    } catch (...) {}
    return std::nullopt;
}

struct ConfigFiles { fs::path source; fs::path target; };

static ConfigFiles resolveConfigFiles() {
    std::string targetStr = getConfigDir() + "config.json";
    if (access(targetStr.c_str(), F_OK) == 0) {
        fs::path target(targetStr);
        return {target, target};
    }
    return {{}, fs::path(targetStr)};
}

struct ConfigDocument {
    Json raw = Json::object(); Json canonical = Json::object();
    fs::path source; fs::path target; bool dirty = false;
};

static ConfigDocument loadConfigDocument() {
    ConfigFiles files = resolveConfigFiles();
    ConfigDocument doc;
    doc.source = files.source;
    doc.target = files.target;
    doc.dirty = doc.source.empty() || (normPath(doc.source) != normPath(doc.target));
    
    if (doc.source.empty()) return doc;
    
    if (access(doc.source.string().c_str(), F_OK) != 0) {
        doc.dirty = true;
        return doc;
    }
    
    try {
        std::ifstream input(doc.source, std::ios::binary);
        if (input.is_open()) {
            Json parsed = Json::parse(input, nullptr, false, true);
            if (!parsed.is_discarded()) {
                doc.raw = parsed;
                doc.canonical = parsed.is_object() ? std::move(parsed) : (doc.dirty = true, Json::object());
            } else { doc.dirty = true; }
        } else { doc.dirty = true; }
    } catch (...) { doc.dirty = true; }
    return doc;
}

static bool saveConfigDocument(const fs::path& path, const Json& config) {
    if (path.empty()) return false;
    
    std::string pathStr = path.string();
    std::string tmpStr = pathStr + ".tmp";
    
    FILE* fp = fopen(tmpStr.c_str(), "wb");
    if (!fp) {
        LOGE("Failed to open tmp file: %s", tmpStr.c_str());
        return false;
    }
    
    std::string payload = config.dump(4);
    size_t written = fwrite(payload.data(), 1, payload.size(), fp);
    fputc('\n', fp);
    fclose(fp);
    
    if (written != payload.size()) {
        unlink(tmpStr.c_str());
        return false;
    }
    
    if (rename(tmpStr.c_str(), pathStr.c_str()) != 0) {
        unlink(tmpStr.c_str());
        return false;
    }
    return true;
}

static void writeMatchedSignature(const std::string& sig, const std::string& detourType) {
    std::string sigPath = getConfigDir() + "signatures.json";
    struct stat st;
    if (stat(sigPath.c_str(), &st) == 0 && st.st_size > 0) return;

    Json sigJson;
    sigJson["signatures"] = Json::array();
    sigJson["signatures"].push_back(sig);
    sigJson["detour"] = detourType;
    saveConfigDocument(fs::path(sigPath), sigJson);
    LOGI("Matched signature saved to signatures.json with detour %s.", detourType.c_str());
}

static void applyConfig(OreUi& ore_ui, const char* label) {
    size_t map_size = ore_ui.mConfigs.size();
    if (map_size == 0) return;
    if (map_size > 2000) {
        LOGE("[%s] Sanity FAILED! mConfigs size = %zu.", label, map_size);
        return;
    }

    g_hookValid = true;
    LOGI("[%s] Valid! Found %zu OreUI configs.", label, map_size);

    ConfigDocument doc = loadConfigDocument();
    if (!doc.canonical.is_object()) { doc.canonical = Json::object(); }

    enum Section { NONE, MODE, SAFE };
    Section current = NONE;
    bool mode_enabled = false;
    std::unordered_map<std::string, bool> mode_entries;
    std::unordered_map<std::string, bool> safe_entries;

    for (auto& [key, value] : doc.canonical.items()) {
        if (key == "mode") {
            if (auto parsed = readBoolLike(value)) mode_enabled = *parsed;
            current = MODE;
            continue;
        }
        if (key == "safe_mode") {
            current = SAFE;
            continue;
        }
        if (auto parsed = readBoolLike(value)) {
            if (current == MODE)       mode_entries[key] = *parsed;
            else if (current == SAFE)  safe_entries[key] = *parsed;
        }
    }

    bool dirty = false;
    for (auto& [name, config] : ore_ui.mConfigs) {
        bool value = false;
        if (mode_enabled && mode_entries.count(name)) {
            value = mode_entries[name];
        } else if (safe_entries.count(name)) {
            value = safe_entries[name];
        }

        if (!doc.canonical.contains(name)) {
            doc.canonical[name] = value;
            dirty = true;
        }

        if (!value) {
            config.mUnknown3 = []() { return false; };
            config.mUnknown4 = []() { return false; };
        }
    }

    if (!doc.canonical.contains("mode")) { doc.canonical["mode"] = true; dirty = true; }
    if (!doc.canonical.contains("safe_mode")) { doc.canonical["safe_mode"] = false; dirty = true; }

    if (dirty) {
        std::string out = "{\n  \"mode\": " + std::string(mode_enabled ? "true" : "false");
        for (auto& [name, _] : ore_ui.mConfigs) {
            if (safe_entries.count(name) && !mode_entries.count(name)) continue;
            bool val = mode_entries.count(name) ? mode_entries.at(name) : false;
            out += ",\n    \"" + name + "\": " + (val ? "true" : "false");
        }
        out += ",\n  \"safe_mode\": false";
        for (auto& [name, _] : ore_ui.mConfigs) {
            if (!safe_entries.count(name)) continue;
            bool val = safe_entries.at(name);
            out += ",\n    \"" + name + "\": " + (val ? "true" : "false");
        }
        out += "\n}\n";

        std::string pathStr = doc.target.string();
        std::string tmpStr  = pathStr + ".tmp";
        FILE* fp = fopen(tmpStr.c_str(), "wb");
        if (fp) {
            fwrite(out.data(), 1, out.size(), fp);
            fclose(fp);
            rename(tmpStr.c_str(), pathStr.c_str());
        }
    }
}

static void ensureConfigExists() {
    std::string configPath  = getConfigDir() + "config.json";
    std::string readmePath  = getConfigDir() + "readme.txt";
    struct stat st;

    if (stat(configPath.c_str(), &st) != 0 || st.st_size == 0) {
        Json defaults = Json::object();
        defaults["mode"] = true;
        defaults["safe_mode"] = false;
        saveConfigDocument(fs::path(configPath), defaults);
    }

    if (stat(readmePath.c_str(), &st) != 0) {
        FILE* fp = fopen(readmePath.c_str(), "w");
        if (fp) {
            fprintf(fp, "=== ForceCloseOreUI Configuration ===\n");
            fclose(fp);
        }
    }
}

// ------------------------------------------------------------
// Hook callbacks
// ------------------------------------------------------------
static void detour_v1(OreUi& a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    orig_v1(a1, a2, a3, a4, a5, a6);
    try { applyConfig(a1, "V1"); } catch (...) {}
}

static void detour_v10(void* a1, void* a2, void* a3, void* a4, void* a5,
                       void* a6, void* a7, void* a8, void* a9, OreUi& a10, void* a11) {
    orig_v10(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    try { applyConfig(a10, "V10"); } catch (...) {}
}

// ------------------------------------------------------------
// Memory scanning
// ------------------------------------------------------------
struct ModuleInfo {
    uintptr_t base;
    size_t size;
};

static bool findMinecraftSegment(ModuleInfo& out) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;

    char line[1024];
    uintptr_t min_base = UINTPTR_MAX;
    uintptr_t max_end = 0;
    bool found_so = false;

    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "r-x")) continue;
        if (strstr(line, "libminecraftpe.so")) {
            uintptr_t start, end;
            if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
                if (start < min_base) min_base = start;
                if (end > max_end) max_end = end;
                found_so = true;
            }
        }
    }

    if (found_so) {
        out.base = min_base;
        out.size = max_end - min_base;
        fclose(fp);
        return true;
    }

    rewind(fp);
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "r-x")) continue;
        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) != 2) continue;
        if ((end - start) > 30 * 1024 * 1024) {
            out.base = start;
            out.size = end - start;
            fclose(fp);
            return true;
        }
    }

    fclose(fp);
    return false;
}

static uintptr_t ResolveSignature(const ModuleInfo& mod, const char* sig) {
    std::vector<int> pattern;
    const char* p = sig;
    while (*p) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { pattern.push_back(-1); p++; if (*p == '?') p++; continue; }
        pattern.push_back(strtol(p, nullptr, 16));
        p += 2;
    }

    if (mod.size < pattern.size()) return 0;
    uint8_t* base = (uint8_t*)mod.base;
    for (size_t i = 0; i <= mod.size - pattern.size(); i += 4) {
        bool found = true;
        for (size_t j = 0; j < pattern.size(); j++) {
            if (pattern[j] != -1 && base[i + j] != (uint8_t)pattern[j]) {
                found = false;
                break;
            }
        }
        if (found) return (uintptr_t)(base + i);
    }
    return 0;
}

// ------------------------------------------------------------
// Hook Installer
// ------------------------------------------------------------
static bool tryInstallHook(const ModuleInfo& mod) {
    std::string sigPath = getConfigDir() + "signatures.json";
    if (access(sigPath.c_str(), F_OK) == 0) {
        try {
            std::ifstream input(sigPath);
            Json json = Json::parse(input, nullptr, false, true);
            if (json.contains("signatures") && json.contains("detour") && json["detour"].is_string()) {
                std::string detourType = json["detour"].get<std::string>();
                std::string sig = json["signatures"][0].get<std::string>();
                uintptr_t addr = ResolveSignature(mod, sig.c_str());
                if (addr != 0) {
                    if (detourType == "V10" && DobbyHook((void*)addr, (void*)detour_v10, (void**)&orig_v10) == 0) return true;
                    if (detourType == "V1"  && DobbyHook((void*)addr, (void*)detour_v1, (void**)&orig_v1) == 0) return true;
                }
            }
        } catch (...) {}
    }

    LOGI("Starting signature scan (V10 / 11 parameters)...");
    for (size_t i = 0; i < SIG_V10.size(); i++) {
        uintptr_t addr = ResolveSignature(mod, SIG_V10[i]);
        if (addr == 0) continue;

        LOGI("V10 Sig[%zu] matched at 0x%lx", i, addr);
        g_hookValid = false;
        
        if (DobbyHook((void*)addr, (void*)detour_v10, (void**)&orig_v10) == 0) {
            for (int w = 0; w < 20 && !g_hookValid; w++) usleep(100'000);
            if (g_hookValid) {
                writeMatchedSignature(std::string(SIG_V10[i]), "V10");
                LOGI("V10 Sig[%zu] VALID!", i);
                return true;
            }
            DobbyDestroy((void*)addr); orig_v10 = nullptr;
        }
    }

    LOGI("Starting signature scan (V1 / 6 parameters)...");
    for (size_t i = 0; i < SIG_V1.size(); i++) {
        uintptr_t addr = ResolveSignature(mod, SIG_V1[i]);
        if (addr == 0) continue;

        LOGI("V1 Sig[%zu] matched at 0x%lx", i, addr);
        g_hookValid = false;
        
        if (DobbyHook((void*)addr, (void*)detour_v1, (void**)&orig_v1) == 0) {
            for (int w = 0; w < 20 && !g_hookValid; w++) usleep(100'000);
            if (g_hookValid) {
                writeMatchedSignature(std::string(SIG_V1[i]), "V1");
                LOGI("V1 Sig[%zu] VALID!", i);
                return true;
            }
            DobbyDestroy((void*)addr); orig_v1 = nullptr;
        }
    }

    LOGI("All signatures exhausted.");
    return false;
}

// ------------------------------------------------------------
// Background threads
// ------------------------------------------------------------
static void* InjectionThread(void*) {
    const int MAX_WAIT = 30000, POLL = 100;
    int waited = 0;
    ModuleInfo mod;

    while (waited < MAX_WAIT) {
        if (findMinecraftSegment(mod)) break;
        usleep(POLL * 1000);
        waited += POLL;
    }

    if (!mod.base) return nullptr;

    for (int attempt = 1; attempt <= 50; attempt++) {
        if (tryInstallHook(mod)) return nullptr;
        usleep(200 * 1000);
    }
    return nullptr;
}

static void* LogcatCaptureThread(void*) {
    std::string logPath = getConfigDir() + "minecraft_runtime.log";
    FILE* clear = fopen(logPath.c_str(), "w");
    if (clear) fclose(clear);

    pid_t pid = getpid();
    char pidStr[32];
    snprintf(pidStr, sizeof(pidStr), "%d", pid);
    std::string lastTimestamp;

    for (int i = 0; i < 1500; i++) {
        int pipefd[2];
        if (pipe(pipefd) != 0) break;

        pid_t child = fork();
        if (child == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            if (lastTimestamp.empty()) {
                execl("/system/bin/logcat", "logcat", "-d", "-v", "threadtime", "-t", "200", "--pid", pidStr, "*:V", (char*)nullptr);
            } else {
                execl("/system/bin/logcat", "logcat", "-d", "-v", "threadtime", "-T", lastTimestamp.c_str(), "--pid", pidStr, "*:V", (char*)nullptr);
            }
            _exit(127);
        }
        close(pipefd[1]);

        FILE* logFile = fopen(logPath.c_str(), "a");
        if (!logFile) {
            close(pipefd[0]);
            kill(child, SIGTERM);
            break;
        }

        std::string lastLine;
        char buf[8192];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            fputs(buf, logFile);
            fflush(logFile);
            char* line = buf;
            while (char* nl = strchr(line, '\n')) {
                *nl = '\0';
                lastLine = line;
                line = nl + 1;
            }
            if (*line != '\0') lastLine = line;
        }
        fclose(logFile);
        close(pipefd[0]);
        int status;
        waitpid(child, &status, 0);

        if (!lastLine.empty() && lastLine.size() > 18) {
            std::string candidate = lastLine.substr(0, 18);
            if (candidate[2] == '-' && candidate[5] == ' ' && candidate[8] == ':') {
                lastTimestamp = candidate;
            }
        }
        usleep(2000 * 1000);
    }
    return nullptr;
}

// ------------------------------------------------------------
// Crash Handlers
// ------------------------------------------------------------
static struct sigaction g_oldSigSegv;
static struct sigaction g_oldSigAbrt;
static struct sigaction g_oldSigBus;

static void crashSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    std::string logPath = getConfigDir() + "crash_native.log";
    int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char buf[1024];
        int len = snprintf(buf, sizeof(buf), "\n[NATIVE CRASH] Signal %d\n", sig);
        write(fd, buf, len);
        close(fd);
    }
    switch (sig) {
        case SIGSEGV: sigaction(SIGSEGV, &g_oldSigSegv, nullptr); break;
        case SIGABRT: sigaction(SIGABRT, &g_oldSigAbrt, nullptr); break;
        case SIGBUS:  sigaction(SIGBUS,  &g_oldSigBus,  nullptr); break;
    }
    raise(sig);
}

static void installCrashHandlers() {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crashSignalHandler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &g_oldSigSegv);
    sigaction(SIGABRT, &sa, &g_oldSigAbrt);
    sigaction(SIGBUS,  &sa, &g_oldSigBus);
}

// ------------------------------------------------------------
// Init
// ------------------------------------------------------------
__attribute__((constructor))
static void ForceCloseOreUI_Init() {
    packageName = getPackageName();
    ensureConfigExists();

    ModuleInfo mod;
    if (findMinecraftSegment(mod)) {
        if (tryInstallHook(mod)) goto start_extras;
    }

    {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, InjectionThread, nullptr) == 0) {
            pthread_detach(thread);
        }
    }

start_extras:
    installCrashHandlers();
    pthread_t logcat_thread;
    if (pthread_create(&logcat_thread, nullptr, LogcatCaptureThread, nullptr) == 0) {
        pthread_detach(logcat_thread);
    }
}