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

// ★ 硬编码保留作为兜底
// 1. 定义带有类型标记的特征码结构
struct FallbackSig {
    const char* sig;
    const char* type; // "V1" 或 "V10"
};

// 2. 替换原有的 SIG_FALLBACK 数组
static const std::vector<FallbackSig> SIG_FALLBACK = {
    // ★ 1.26.40+ 新版本 V10 (我们刚刚定位出来的那个)
    {"? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 5B D0 3B D5 F4 03 03 2A E3 03 02 AA 68 17 40 F9 E2 03 01 AA ? ? ? D1 A4 0C 80 52 F3 03 00 AA A8 83 1F F8 03 85 FF 97", "V10"},
    
    // ★ 1.26.40+ 新版本 V1 (QuickJS 引擎相关)
    {"? ? ? D1 ? ? ? A9 F9 13 00 F9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 F7 03 05 AA F8 03 04 2A E4 03 01 AA E5 03 02 AA E6 03 1F 2A F3 03 02 AA F4 03 01 AA F5 03 00 AA 00 C0 92 95 F6 03 01 AA F9 03 00 AA", "V1"},
    
    // 下面是以前版本的旧特征码，我也帮你标记好了对应的类型
    {"? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 F7 03 05 AA FB 03 03 2A", "V10"},
    {"? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? D5 FB 03 03 2A F8 03 02 2A", "V10"},
    {"? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA", "V10"},
    {"? ? ? D1 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? 91 ? ? ? F9 ? ? ? D5 FB 03 00 AA ? ? ? F9 F5 03 07 AA", "V10"},
    {"? ? ? D1 ? ? ? A9 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA", "V10"},
    {"? ? ? D1 ? ? ? 91 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 E8 03 03 AA", "V10"},
    {"? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 ? ? ? A9 FD 03 00 91 ? ? ? D1 ? ? ? D5 FB 03 00 AA F5 03 07 AA", "V10"}
};

static std::string g_loadedDetourType = "";
static std::vector<std::string> g_loadedSignatures;

static std::vector<std::string>& loadSignatures() {
    if (!g_loadedSignatures.empty()) return g_loadedSignatures;

    std::string sigPath = getConfigDir() + "signatures.json";

    try {
        if (access(sigPath.c_str(), F_OK) == 0) {
            std::ifstream input(sigPath);
            Json json = Json::parse(input, nullptr, false, true);
            if (!json.is_discarded() && json.contains("signatures") && json["signatures"].is_array()) {
                for (auto& s : json["signatures"]) {
                    if (s.is_string()) g_loadedSignatures.push_back(s.get<std::string>());
                }
                if (json.contains("detour") && json["detour"].is_string()) {
                    g_loadedDetourType = json["detour"].get<std::string>();
                }
                if (!g_loadedSignatures.empty()) {
                    LOGI("Loaded %zu signatures from %s (detour=%s)", 
                         g_loadedSignatures.size(), sigPath.c_str(), g_loadedDetourType.c_str());
                    return g_loadedSignatures;
                }
            }
        }
    } catch (...) {}

    LOGI("Using built-in fallback signatures.");
    // ★ 关键修改点：从 .sig 成员中读取字符串
    for (const auto& s : SIG_FALLBACK) g_loadedSignatures.push_back(s.sig);
    g_loadedDetourType = "";
    return g_loadedSignatures;
}





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

static void writeMatchedSignature(const std::string& sig, const std::string& detourType) {
    std::string sigPath = getConfigDir() + "signatures.json";
    struct stat st;
    if (stat(sigPath.c_str(), &st) == 0 && st.st_size > 0) return;

    Json sigJson;
    sigJson["signatures"] = Json::array();
    sigJson["signatures"].push_back(sig);
    sigJson["detour"] = detourType; // 存入 detour 类型
    saveConfigDocument(fs::path(sigPath), sigJson);
    LOGI("Matched signature saved to signatures.json with detour %s.", detourType.c_str());
}

// ------------------------------------------------------------
// applyConfig（带 Sanity Check）
// ------------------------------------------------------------
static void applyConfig(OreUi& ore_ui, const char* label) {
    size_t map_size = ore_ui.mConfigs.size();
    if (map_size == 0) {
        LOGI("[%s] mConfigs empty (size 0). Skipping.", label);
        return;
    }
    if (map_size > 2000) {
        LOGE("[%s] Sanity FAILED! mConfigs size = %zu.", label, map_size);
        return;
    }

    g_hookValid = true;
    LOGI("[%s] Valid! Found %zu OreUI configs.", label, map_size);

    ConfigDocument doc = loadConfigDocument();
    if (!doc.canonical.is_object()) { doc.canonical = Json::object(); }

    // ★ 扁平分段解析
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
        // 普通条目，归当前区域
        if (auto parsed = readBoolLike(value)) {
            if (current == MODE)       mode_entries[key] = *parsed;
            else if (current == SAFE)  safe_entries[key] = *parsed;
        }
    }

    // ★ safe_mode 条目永远生效，不需要全局开关
    LOGI("[%s] mode=%s", label, mode_enabled ? "ON" : "OFF");

    std::vector<std::string> keysOrder; // 保持 JSON 顺序
    bool dirty = false;

    for (auto& [name, config] : ore_ui.mConfigs) {
        keysOrder.push_back(name);
        LOGI("[%s] -> %s", label, name.c_str());

        bool value = false;

        // 优先级：mode 条目（且 mode=true） > safe_mode 条目（永远生效）
        if (mode_enabled && mode_entries.count(name)) {
            value = mode_entries[name];
        } else if (safe_entries.count(name)) {
            value = safe_entries[name];  // ★ safe_mode 条目不受全局开关影响
        }

        // ★ 回写：确保 config.json 里每个 OreUI 条目都存在
        if (!doc.canonical.contains(name)) {
            // 新条目，根据所在区域决定写到哪里
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

// 确保 mode 和 safe_mode 标记存在
if (!doc.canonical.contains("mode")) { doc.canonical["mode"] = true; dirty = true; }
if (!doc.canonical.contains("safe_mode")) { doc.canonical["safe_mode"] = false; dirty = true; }

if (dirty) {
    // 手动拼 flat-sectioned JSON，完美还原主人的格式
    std::string out = "{\n";

    // ── mode 行 ──────────────────────────────────
    out += "  \"mode\": ";
    out += mode_enabled ? "true" : "false";

    // mode 区段条目（不是纯 safe 条目的都写在这里）
    for (auto& [name, _] : ore_ui.mConfigs) {
        bool inSafeOnly = safe_entries.count(name) && !mode_entries.count(name);
        if (inSafeOnly) continue;
        bool val = mode_entries.count(name) ? mode_entries.at(name) : false;
        out += ",\n    \"" + name + "\": ";
        out += val ? "true" : "false";
    }

    // ── safe_mode 行 ─────────────────────────────
    out += ",\n  \"safe_mode\": false";

    // safe 区段条目（允许和 mode 区重复，如 /play）
    for (auto& [name, _] : ore_ui.mConfigs) {
        if (!safe_entries.count(name)) continue;
        bool val = safe_entries.at(name);
        out += ",\n    \"" + name + "\": ";
        out += val ? "true" : "false";
    }

    out += "\n}\n";

    // 复用现有 atomic write 逻辑
    std::string pathStr = doc.target.string();
    std::string tmpStr  = pathStr + ".tmp";
    FILE* fp = fopen(tmpStr.c_str(), "wb");
    if (fp) {
        fwrite(out.data(), 1, out.size(), fp);
        fclose(fp);
        if (rename(tmpStr.c_str(), pathStr.c_str()) == 0)
            LOGI("[%s] Config saved (sectioned format, %zu bytes).", label, out.size());
        else
            LOGE("[%s] rename failed: %s", label, strerror(errno));
    } else {
        LOGE("[%s] fopen tmp failed: %s", label, strerror(errno));
    }
}
}


static void ensureConfigExists() {
    std::string configPath  = getConfigDir() + "config.json";
    std::string sigPath     = getConfigDir() + "signatures.json";
    std::string readmePath  = getConfigDir() + "readme.txt";
    struct stat st;

    // --- config.json（最小结构，条目录由 applyConfig 补全） ---
    if (stat(configPath.c_str(), &st) != 0 || st.st_size == 0) {
        Json defaults = Json::object();
        defaults["mode"] = true;
        defaults["safe_mode"] = false;  // ★ 只是一个分段标记，不是全局开关
        saveConfigDocument(fs::path(configPath), defaults);
        LOGI("config.json created (entries will be filled on first hook).");
    }

    // --- readme.txt ---
    if (stat(readmePath.c_str(), &st) != 0) {
        FILE* fp = fopen(readmePath.c_str(), "w");
        if (fp) {
            fprintf(fp,
                "=== ForceCloseOreUI 配置文件说明 ===\n\n"
                "【 config.json 】\n"
                "  分段式结构，按\"区域标记行\"分割：\n\n"
                "  \"mode\": true          ← 正常模式区域开关\n"
                "    /play: true          ← 游戏界面：保留 OreUI\n"
                "    /settings: false     ← 设置界面：关闭 OreUI（用旧版UI）\n\n"
                "  \"safe_mode\": false    ← 强制模式区域标记（不是全局开关！）\n"
                "    /achievement: false  ← 成就界面：强制关闭 OreUI\n\n"
                "  ★ mode 和 safe_mode 都只是分段标记，不控制其他条目\n"
                "  ★ safe_mode 下的条目一定会生效（强制模式）\n"
                "  ★ mode 下的条目：mode 为 true 时生效\n"
                "  ★ 两个区域都没有的条目：默认关闭 OreUI\n"
                "  ★ 同一个条目同时出现在两个区域：mode 优先\n\n"
                "【 signatures.json 】\n"
                "  ARM64 内存特征码，新版 Minecraft 需要加新特征码时在这里添加\n"
                "  SO 内置了兜底签名，删掉此文件会自动重新生成\n"
            );
            fclose(fp);
        }
        LOGI("readme.txt created.");
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
    auto& sigs = loadSignatures();
    if (sigs.empty()) return false;

    bool isTrusted = false;
    std::string sigPath = getConfigDir() + "signatures.json";
    if (access(sigPath.c_str(), F_OK) == 0 && !g_loadedDetourType.empty()) {
        isTrusted = true;
    }

    std::vector<const char*> sigPtrs;
    for (auto& s : sigs) sigPtrs.push_back(s.c_str());

    LOGI("Starting signature scan (%zu sigs, trusted=%s)...", sigs.size(), isTrusted ? "YES" : "NO");

    for (size_t i = 0; i < sigPtrs.size(); i++) {
        uintptr_t addr = ResolveSignature(mod, sigPtrs[i]);
        if (addr == 0) continue;

        LOGI("Sig[%zu] matched at 0x%lx", i, addr);

        if (isTrusted) {
            if (g_loadedDetourType == "V1") {
                if (DobbyHook((void*)addr, (void*)detour_v1, (void**)&orig_v1) == 0) {
                    LOGI("Sig[%zu] → V1 Hooked (Trusted, skip validation)", i);
                    return true;
                }
            } else if (g_loadedDetourType == "V10") {
                if (DobbyHook((void*)addr, (void*)detour_v10, (void**)&orig_v10) == 0) {
                    LOGI("Sig[%zu] → V10 Hooked (Trusted, skip validation)", i);
                    return true;
                }
            }
            continue;
        }

        // --- 动态扫描 Fallback 的逻辑进行修正 ---
        // 获取当前特征码预设的正确类型
        std::string expectedType = "V10"; // 默认兜底
        if (i < SIG_FALLBACK.size()) {
            expectedType = SIG_FALLBACK[i].type;
        }

        if (expectedType == "V1") {
            g_hookValid = false;
            if (DobbyHook((void*)addr, (void*)detour_v1, (void**)&orig_v1) == 0) {
                for (int w = 0; w < 20 && !g_hookValid; w++) usleep(100'000);
                if (g_hookValid) {
                    writeMatchedSignature(std::string(sigPtrs[i]), "V1");
                    LOGI("Sig[%zu] -> V1 VALID!", i);
                    return true;
                }
                DobbyDestroy((void*)addr); orig_v1 = nullptr;
            }
        } else if (expectedType == "V10") {
            g_hookValid = false;
            if (DobbyHook((void*)addr, (void*)detour_v10, (void**)&orig_v10) == 0) {
                for (int w = 0; w < 20 && !g_hookValid; w++) usleep(100'000);
                if (g_hookValid) {
                    writeMatchedSignature(std::string(sigPtrs[i]), "V10");
                    LOGI("Sig[%zu] -> V10 VALID!", i);
                    return true;
                }
                DobbyDestroy((void*)addr); orig_v10 = nullptr;
            }
        }
    }

    LOGI("All signatures exhausted.");
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