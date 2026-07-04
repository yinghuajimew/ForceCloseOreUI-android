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
using Json = nlohmann::json;

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
    
    // 1. 总是输出到 logcat
    int prio = (level[0] == 'E') ? ANDROID_LOG_ERROR : ANDROID_LOG_INFO;
    __android_log_print(prio, "ForceCloseOreUI", "[%s] %s", level, buf);
    
    // 2. 只有路径已确定时才写文件
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
// Signatures — 保持原始版本不变
// V1: 旧版本，多参数，OreUi 在第 1 个
// V10: 新版本，11 参数，OreUi 在第 10 个
// ------------------------------------------------------------
// V1: 旧版本，多参数，OreUi 在第 1 个
static const std::vector<const char*> SIG_V1 = {
    //1.26.30
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F7 03 05 AA FB 03 03 2A",
    // H8 - 1.26.20 最新
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FB 03 03 2A F8 03 02 2A",
    // H4 - 1.21.130 ~ 1.26.0
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA",
    // H5 - 1.26.0 ~ 1.26.10
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? F9 ? ? ? D5 FB 03 00 AA ? ? ? F9 F5 03 07 AA",
    // ★ 补H2 - 1.21.90 ~ 1.21.120
    "? ? ? D1 ? ? ? A9 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA",
    // ★ 补H3 - 1.21.120
    "? ? ? D1 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA",
    // ★ 补H7 - 1.26.10
    "? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA",
};

static const std::vector<const char*> SIG_V10 = {
//1.26.30
    "? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F7 03 05 AA FB 03 03 2A",
};

// ------------------------------------------------------------
// 自验证标志（Hook 回调写入，tryHookGroup 读取）
// ------------------------------------------------------------
static volatile bool g_hookValid = false;

// ------------------------------------------------------------
// 智能模块定位
// ------------------------------------------------------------
struct ModuleInfo {
    uintptr_t base;
    size_t size;
};

static bool findMinecraftSegment(ModuleInfo& out) {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return false;

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "r-x")) continue;
        if (!strstr(line, "libminecraftpe.so")) continue;

        uintptr_t start, end;
        if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
            out.base = start;
            out.size = end - start;
            fclose(fp);
            return true;
        }
    }

    // 兜底：extractNativeLibs="false" 时 SO 映射为 base.apk
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

// ------------------------------------------------------------
// Memory scanning
// ------------------------------------------------------------
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
// Hook function pointers
// ------------------------------------------------------------
typedef void (*OreUiInitFuncV1)(OreUi&, void*, void*, void*, void*, void*);
static OreUiInitFuncV1 orig_v1 = nullptr;

typedef void (*OreUiInitFuncV10)(void*, void*, void*, void*, void*, void*, void*, void*, void*, OreUi&, void*);
static OreUiInitFuncV10 orig_v10 = nullptr;

// ------------------------------------------------------------
// Configuration
// ------------------------------------------------------------
static std::string getPackageName() {
    std::ifstream cmdline("/proc/self/cmdline");
    std::string pkg;
    std::getline(cmdline, pkg, '\0');
    return pkg.empty() ? "com.mojang.minecraftpe" : pkg;
}

static std::string getConfigDir() {
    static std::string cached;  // 静态变量，只初始化一次
    if (!cached.empty()) return cached;  // 如果已经缓存了，直接返回
    
    // 下面才是第一次调用时执行的逻辑
    const std::string& pkg = packageName.empty() ? getPackageName() : packageName;
    
    // 候选路径（按优先级）
    std::vector<std::string> candidates = {
        "/storage/emulated/0/games/com.mojang/ForceCloseOreUI/",           // Android 10-
        "/storage/emulated/0/Android/data/" + pkg + "/mods/ForceCloseOreUI/", // Android 11+
    };
    
    for (const auto& base : candidates) {
        // 提取父目录路径
        size_t lastSlash = base.find_last_of('/', base.size() - 2);
        std::string parent1 = base.substr(0, lastSlash);
        lastSlash = parent1.find_last_of('/');
        std::string parent2 = parent1.substr(0, lastSlash);
        
        // 逐级创建
        mkdir(parent2.c_str(), 0755);
        mkdir(parent1.c_str(), 0755);
        mkdir(base.c_str(), 0755);
        
        // 验证是否可写（尝试创建测试文件）
        std::string testFile = base + ".test";
        FILE* fp = fopen(testFile.c_str(), "wb");
        if (fp) {
            fclose(fp);
            unlink(testFile.c_str());
            cached = base;
            g_logFilePath = base + "debug.log";  // ★ 设置日志路径
            LOGI("Using config dir: %s", base.c_str());  // 现在可以安全调用了
            return cached;
        }
        
        LOGI("Path not writable: %s (errno=%d: %s)", base.c_str(), errno, strerror(errno));
    }
    
    // 所有外部路径都失败，用内部存储兜底
    std::string fallback = "/data/data/" + pkg + "/files/ForceCloseOreUI/";
    mkdir(("/data/data/" + pkg + "/files").c_str(), 0755);
    mkdir(fallback.c_str(), 0755);
    cached = fallback;
    g_logFilePath = fallback + "debug.log";  // ★ 设置日志路径
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

static std::optional<bool> readNamedConfigValue(const Json& root, const std::string& name) {
    if (root.is_object()) {
        auto direct = root.find(name);
        if (direct != root.end()) { if (auto parsed = readBoolLike(*direct)) return parsed; }
        for (std::string_view key : {"configs", "settings", "toggles", "values", "oreui", "OreUI"}) {
            auto section = root.find(std::string(key));
            if (section != root.end() && section->is_object()) {
                auto nested = section->find(name);
                if (nested != section->end()) { if (auto parsed = readBoolLike(*nested)) return parsed; }
            }
        }
    }
    if (root.is_array()) {
        for (const Json& item : root) {
            if (!item.is_object()) continue;
            bool name_matched = false;
            for (std::string_view key : {"name", "key", "id"}) {
                auto name_node = item.find(std::string(key));
                if (name_node != item.end() && name_node->is_string() &&
                    name_node->get<std::string>() == name) { name_matched = true; break; }
            }
            if (name_matched) { if (auto parsed = readBoolLike(item)) return parsed; }
        }
    }
    return std::nullopt;
}

struct ConfigFiles { fs::path source; fs::path target; };

static ConfigFiles resolveConfigFiles() {
    std::string targetStr = getConfigDir() + "config.json";
    // 用 access() 代替 fs::exists()
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
    
    // 用 access() 检查
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
    
    // 用纯 C 的 fopen/fwrite（兼容所有 Android 版本）
    FILE* fp = fopen(tmpStr.c_str(), "wb");
    if (!fp) {
        LOGE("Failed to open tmp file: %s (errno=%d: %s)", 
             tmpStr.c_str(), errno, strerror(errno));
        return false;
    }
    
    std::string payload = config.dump(4);
    size_t written = fwrite(payload.data(), 1, payload.size(), fp);
    fputc('\n', fp);
    fclose(fp);
    
    if (written != payload.size()) {
        LOGE("Incomplete write: %zu/%zu bytes", written, payload.size());
        unlink(tmpStr.c_str());
        return false;
    }
    
    // ★ 新增：验证临时文件存在
struct stat st;
if (stat(tmpStr.c_str(), &st) != 0) {
    LOGE("Temp file doesn't exist after write: %s", tmpStr.c_str());
    return false;
}
    
    // rename 是原子操作，在所有 Android 版本上都可靠
    if (rename(tmpStr.c_str(), pathStr.c_str()) != 0) {
        LOGE("rename failed: %s -> %s (errno=%d: %s)",
             tmpStr.c_str(), pathStr.c_str(), errno, strerror(errno));
        unlink(tmpStr.c_str());
        return false;
    }
    
    // ★ 新增：验证最终文件存在且可读
    if (stat(pathStr.c_str(), &st) != 0) {
        LOGE("CRITICAL: File doesn't exist after rename: %s", pathStr.c_str());
        return false;
    }
    
    LOGI("Config saved successfully: %s (%ld bytes)", pathStr.c_str(), (long)st.st_size);
    return true;
}

// ------------------------------------------------------------
// applyConfig（带 Sanity Check）
// ------------------------------------------------------------
static void applyConfig(OreUi& ore_ui, const char* label) {
    LOGI("[%s] applyConfig called! mConfigs.size()=%zu &ore_ui=%p",
         label, ore_ui.mConfigs.size(), &ore_ui);

    size_t map_size = ore_ui.mConfigs.size();
    if (map_size == 0) {
        LOGI("[%s] mConfigs empty (size 0). Skipping.", label);
        return;
    }
    if (map_size > 2000) {
        LOGE("[%s] Sanity FAILED! mConfigs size = %zu (garbage). Wrong function hooked!", label, map_size);
        return;
    }

    g_hookValid = true;
    LOGI("[%s] Valid! Found %zu OreUI configs.", label, map_size);

    ConfigDocument doc = loadConfigDocument();
    bool dirty = doc.dirty || access(doc.target.string().c_str(), F_OK) != 0 ;
    if (!doc.canonical.is_object()) { doc.canonical = Json::object(); dirty = true; }

    // 安全模式检查
    bool safe_mode = false;
    auto sm = doc.canonical.find("safe_mode");
    if (sm != doc.canonical.end()) {
        if (auto parsed = readBoolLike(*sm)) safe_mode = *parsed;
    }

    if (safe_mode) {
        LOGI("[%s] SAFE MODE enabled! Forcing ALL OreUI off.", label);
    }

    for (auto& [name, config] : ore_ui.mConfigs) {
        LOGI("[%s] -> %s", label, name.c_str());

        bool value = false;
        bool rewrite = false;

        if (!safe_mode) {
            // 只在非安全模式下读取各条目的值
            auto existing = doc.canonical.find(name);
            if (existing != doc.canonical.end()) {
                if (auto parsed = readBoolLike(*existing)) {
                    value = *parsed;
                    rewrite = !existing->is_boolean();
                } else {
                    if (auto legacy = readNamedConfigValue(doc.raw, name))
                        value = *legacy;
                    rewrite = true;
                }
            } else {
                if (auto legacy = readNamedConfigValue(doc.raw, name))
                    value = *legacy;
                rewrite = true;
            }
        }

        // 新条目或需要重写时，记录到 config
        if (rewrite || doc.canonical.find(name) == doc.canonical.end()) {
            doc.canonical[name] = value;
            dirty = true;
        }

        if (!value) {
            config.mUnknown3 = []() { return false; };
            config.mUnknown4 = []() { return false; };
            LOGI("[%s]   -> Disabled: %s", label, name.c_str());
        } else {
            LOGI("[%s]   -> Kept OreUI: %s", label, name.c_str());
        }
    }

    // 确保 safe_mode 字段始终存在
    if (doc.canonical.find("safe_mode") == doc.canonical.end()) {
        doc.canonical["safe_mode"] = true;
        dirty = true;
    }

    if (dirty) {
        if (saveConfigDocument(doc.target, doc.canonical)) {
            LOGI("[%s] Config saved (safe_mode=%s)", label, safe_mode ? "true" : "false");
        } else {
            LOGE("Config applied in memory but could not be saved.");
        }
    }
}


static void ensureConfigExists() {
    std::string configPath = getConfigDir() + "config.json";
    
    LOGI("Checking config: %s", configPath.c_str());
    
    // 用 stat() 检查（比 access 更可靠）
    struct stat st;  // ← 只在这里声明一次
    if (stat(configPath.c_str(), &st) == 0 && st.st_size > 0) {
        LOGI("Config exists (%ld bytes)", (long)st.st_size);
        return;
    }
    
    // 创建默认配置
    Json defaults = Json::object();
    defaults["safe_mode"] = true;
    defaults["_comment"] = "safe_mode=true: all OreUI disabled. "
                           "safe_mode=false: use per-entry toggles below.";
    
    bool ok = saveConfigDocument(fs::path(configPath), defaults);
    
    if (!ok) {
        LOGE("External storage write FAILED. Trying internal storage fallback...");
        
        // 兜底：写入内部存储
        std::string internalPath = "/data/data/" + packageName + "/files/ForceCloseOreUI_config.json";
        mkdir(("/data/data/" + packageName + "/files").c_str(), 0755);
        
        FILE* fp = fopen(internalPath.c_str(), "wb");
        if (fp) {
            std::string payload = defaults.dump(4);
            fwrite(payload.data(), 1, payload.size(), fp);
            fclose(fp);
            LOGI("Fallback config created at: %s", internalPath.c_str());
        } else {
            LOGE("CRITICAL: Both external and internal storage failed!");
        }
    }
    
    // 最终验证（复用前面的 st 变量，不需要再声明）
    if (stat(configPath.c_str(), &st) == 0) {  // ← 这里直接用，不要再写 struct stat st;
        LOGI("Verified: config exists on disk (%ld bytes)", (long)st.st_size);
    } else {
        LOGE("Config creation FAILED on this device!");
    }
}

// ------------------------------------------------------------
// Hook callbacks
// ------------------------------------------------------------
static void detour_v1(OreUi& a1, void* a2, void* a3, void* a4, void* a5, void* a6) {
    orig_v1(a1, a2, a3, a4, a5, a6);
    try { applyConfig(a1, "V1"); } catch (...) { LOGE("[V1] exception"); }
}

static void detour_v10(void* a1, void* a2, void* a3, void* a4, void* a5,
                       void* a6, void* a7, void* a8, void* a9, OreUi& a10, void* a11) {
    orig_v10(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11);
    try { applyConfig(a10, "V10"); } catch (...) { LOGE("[V10] exception"); }
}

// ------------------------------------------------------------
// 自验证 Hook 安装：安装后立刻调原函数一次，
// 如果 Sanity Check 失败就自动卸载，继续尝试下一条签名
// ------------------------------------------------------------
template<typename OrigPtr>
static bool tryHookGroup(const ModuleInfo& mod, const std::vector<const char*>& sigs,
                         void* detour, OrigPtr** orig_out,
                         const char* label) {
    for (size_t i = 0; i < sigs.size(); i++) {
        LOGI("  Trying %s [%zu/%zu]...", label, i + 1, sigs.size());
        uintptr_t addr = ResolveSignature(mod, sigs[i]);
        if (addr == 0) {
            LOGI("  %s [%zu] -> not found.", label, i + 1);
            continue;
        }

        // 打印匹配地址处的前 16 字节，方便用户用 IDA 核实
        uint8_t* raw = (uint8_t*)addr;
        LOGI("  %s [%zu] -> MATCHED at 0x%lx", label, i + 1, addr);
        LOGI("    Bytes: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7],
             raw[8], raw[9], raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);

        // 重置自验证标志
        g_hookValid = false;

        if (DobbyHook((void*)addr, detour, (void**)orig_out) != 0) {
            LOGE("  DobbyHook failed at 0x%lx", addr);
            continue;
        }

        LOGI("  DobbyHook installed. Triggering validation...");

        // 调用原函数一次来触发 Sanity Check
        // 注意：只有当这个函数会被游戏启动阶段调用时才能验证
        // 给 2 秒时间等待回调触发
        for (int w = 0; w < 20; w++) {
            if (g_hookValid) break;
            usleep(100 * 1000);
        }

        if (g_hookValid) {
            LOGI("  %s [%zu] -> VALID hook! Confirmed correct function.", label, i + 1);
            return true;
        }

        // Sanity Check 没通过 → 卸载这个错误的 Hook
        LOGE("  %s [%zu] -> INVALID hook (validation failed). Unhooking and trying next...", label, i + 1);
        DobbyDestroy((void*)addr);
        *orig_out = nullptr;
    }
    return false;
}

static bool tryInstallHook(const ModuleInfo& mod) {
    LOGI("Starting signature scan (module: 0x%lx, %zu bytes)...", mod.base, mod.size);
    // ★ 保持原始优先级：先 V1，再 V10
    if (tryHookGroup(mod, SIG_V1, (void*)detour_v1, &orig_v1, "V1"))
        return true;
    if (tryHookGroup(mod, SIG_V10, (void*)detour_v10, &orig_v10, "V10"))
        return true;
    LOGI("All signatures exhausted, no valid hook found.");
    return false;
}

// ------------------------------------------------------------
// Background thread
// ------------------------------------------------------------
static void* InjectionThread(void*) {
    LOGI("=== Background thread STARTED (tid=%d) ===", gettid());

    const int MAX_WAIT = 30000, POLL = 100;
    int waited = 0;
    ModuleInfo mod;

    while (waited < MAX_WAIT) {
        if (findMinecraftSegment(mod)) {
            LOGI("Engine found at 0x%lx (%zu bytes) after %d ms", mod.base, mod.size, waited);
            break;
        }
        if (waited % 1000 == 0) LOGI("Waiting... (%d ms)", waited);
        usleep(POLL * 1000);
        waited += POLL;
    }

    if (!mod.base) {
        LOGE("Engine not found after %d ms.", MAX_WAIT);
        return nullptr;
    }

    for (int attempt = 1; attempt <= 50; attempt++) {
        LOGI("=== Attempt %d/50 ===", attempt);
        if (tryInstallHook(mod)) {
            LOGI("=== Hook verified on attempt %d! ===", attempt);
            return nullptr;
        }
        usleep(200 * 1000);
    }

    LOGE("Failed after 50 attempts. No signature matched this version.");
    return nullptr;
}

// ------------------------------------------------------------
// Minecraft 运行日志抓取（logcat）
// ------------------------------------------------------------
static void* LogcatCaptureThread(void*) {
    LOGI("Logcat snapshot thread started.");

    std::string logPath = getConfigDir() + "minecraft_runtime.log";
    FILE* clear = fopen(logPath.c_str(), "w");
    if (clear) fclose(clear);

    pid_t pid = getpid();
    char pidStr[32];
    snprintf(pidStr, sizeof(pidStr), "%d", pid);

    std::string lastTimestamp; // 记录上次读到的最后一条时间戳

    for (int i = 0; i < 1500; i++) { // 最多 50 分钟
        int pipefd[2];
        if (pipe(pipefd) != 0) break;

        pid_t child = fork();
        if (child == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            if (lastTimestamp.empty()) {
                // 第一次：取最近 200 条
                execl("/system/bin/logcat", "logcat",
                      "-d", "-v", "threadtime",
                      "-t", "200",
                      "--pid", pidStr,
                      "*:V", (char*)nullptr);
            } else {
                // 后续：只取比上次时间戳更新的条目
                // -T <timestamp> 表示从该时间戳开始输出
                execl("/system/bin/logcat", "logcat",
                      "-d", "-v", "threadtime",
                      "-T", lastTimestamp.c_str(),
                      "--pid", pidStr,
                      "*:V", (char*)nullptr);
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

        // 先读到内存，同时提取最后一条的时间戳
        std::string lastLine;
        char buf[8192];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';

            // 追加写入文件
            fputs(buf, logFile);
            fflush(logFile);

            // 提取最后一行用于下次时间戳
            char* line = buf;
            while (char* nl = strchr(line, '\n')) {
                *nl = '\0';
                lastLine = line;
                line = nl + 1;
            }
            // 剩余没换行的部分也记下来
            if (*line != '\0') lastLine = line;
        }

        fclose(logFile);
        close(pipefd[0]);

        int status;
        waitpid(child, &status, 0);

        // 从最后一行提取时间戳
        // logcat threadtime 格式: "05-16 20:44:58.408 13644 13737 E TAG : msg"
        // 时间戳就是 "05-16 20:44:58.408"
        if (!lastLine.empty() && lastLine.size() > 18) {
            // 格式: "MM-DD HH:MM:SS.mmm"
            std::string candidate = lastLine.substr(0, 18);
            if (candidate[2] == '-' && candidate[5] == ' ' && candidate[8] == ':') {
                lastTimestamp = candidate;
            }
        }

        usleep(2000 * 1000); // 每 2 秒一次
    }

    LOGI("Logcat snapshot thread stopped.");
    return nullptr;
}

// ------------------------------------------------------------
// Native Crash 信号捕获
// ------------------------------------------------------------
static struct sigaction g_oldSigSegv;
static struct sigaction g_oldSigAbrt;
static struct sigaction g_oldSigBus;

static const char* sigToStr(int sig) {
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation Fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGBUS:  return "SIGBUS (Bus Error)";
        default:      return "Unknown Signal";
    }
}

static void crashSignalHandler(int sig, siginfo_t* info, void* ucontext) {
    // 在信号处理器中只能用 async-signal-safe 函数，不能用 LOGI
    // 直接用 write() 写文件
    std::string logPath = getConfigDir() + "crash_native.log";

    int fd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) {
        char buf[1024];
        int len;

        // 写入崩溃头
        len = snprintf(buf, sizeof(buf),
            "\n========================================\n"
            "[NATIVE CRASH] %s\n"
            "Fault address: %p\n"
            "Signal code:   %d\n"
            "Process PID:   %d\n"
            "Timestamp:     %ld\n"
            "========================================\n",
            sigToStr(sig),
            info->si_addr,
            info->si_code,
            getpid(),
            (long)time(nullptr)
        );
        write(fd, buf, len);

        // 尝试打印 backtrace（简单版：读 /proc/self/maps 中 PC 附近的映射）
        ucontext_t* uc = (ucontext_t*)ucontext;
        uintptr_t pc = uc->uc_mcontext.pc;
        len = snprintf(buf, sizeof(buf), "PC: 0x%lx\n", (unsigned long)pc);
        write(fd, buf, len);

        // 打印寄存器快照（ARM64）
        for (int i = 0; i < 31; i++) {
            len = snprintf(buf, sizeof(buf), "X%-2d: 0x%lx\n", i,
                           (unsigned long)uc->uc_mcontext.regs[i]);
            write(fd, buf, len);
        }
        len = snprintf(buf, sizeof(buf), "SP:  0x%lx\nLR:  0x%lx\n",
                       (unsigned long)uc->uc_mcontext.sp,
                       (unsigned long)uc->uc_mcontext.regs[30]);
        write(fd, buf, len);

        close(fd);
    }

    // 恢复原始信号处理器并重新抛出信号，让系统生成 tombstone
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

    LOGI("Native crash signal handlers installed.");
}

// ------------------------------------------------------------
// Constructor
// ------------------------------------------------------------
__attribute__((constructor))
static void ForceCloseOreUI_Init() {
    packageName = getPackageName();

    LOGI("=== ForceCloseOreUI Mod Started ===");
    LOGI("Package: %s", packageName.c_str());
    
    ensureConfigExists();

    ModuleInfo mod;
    if (findMinecraftSegment(mod)) {
        LOGI("Engine loaded at 0x%lx (%zu bytes). Sync hook...", mod.base, mod.size);
        if (tryInstallHook(mod)) {
            LOGI("Sync hook verified!");
            goto start_extras;  // ★ 成功也要启动额外任务
        }
        LOGI("Sync hook failed. Falling back to thread...");
    } else {
        LOGI("Engine not loaded yet.");
    }

    {
        pthread_t thread;
        int ret = pthread_create(&thread, nullptr, InjectionThread, nullptr);
        if (ret != 0) LOGE("pthread_create failed: %d", ret);
        else pthread_detach(thread);
    }

start_extras:
    // ★ 安装崩溃信号处理器
    installCrashHandlers();

    // ★ 启动 logcat 抓取线程（持续运行直到游戏退出）
    {
        pthread_t logcat_thread;
        int ret = pthread_create(&logcat_thread, nullptr, LogcatCaptureThread, nullptr);
        if (ret != 0) LOGE("Logcat thread create failed: %d", ret);
        else pthread_detach(logcat_thread);
    }
}