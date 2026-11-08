// ------------------------------------------------------------
// 从 APK 的 AndroidManifest.xml 读 versionName
// ------------------------------------------------------------
// 为什么不用别的办法：
//   * /proc/self/maps 里只有安装路径的 hash（包名-xxxx），不是 "1.26.23"
//   * 游戏自己的 logcat 里确实有 clientVersion=1.26.23，但那要等它发网络
//     请求时才打印，通常已经过启动后 8 秒 —— 依赖版 2~3 秒就崩了，等不到
//   * 所以只能自己解开 APK 读清单。这里不引第三方库，ZIP 只认
//     stored / deflate 两种（APK 的清单只可能是这两种之一），
//     AXML 只解出 versionName 一个属性。
//
// ★ 绝不能把整个 APK 读进内存：MC 的 base.apk 是 GB 级的，
//   所以下面全程用 fseek/fread 只碰 EOCD、中央目录和目标条目这三段。
//
// 全程纯读文件，不碰 JVM，不依赖 JNI 是否就绪。
#pragma once

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <zlib.h>

namespace mcver {

static inline uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static inline uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// 带边界的随机读，失败返回 false
static inline bool pread_at(FILE* f, uint64_t off, void* buf, size_t n) {
    if (fseeko(f, (off_t)off, SEEK_SET) != 0) return false;
    return fread(buf, 1, n, f) == n;
}

// ---------- AXML 字符串池 ----------

static inline std::string poolString(const std::vector<uint8_t>& a, size_t poolOff,
                                     bool utf8, uint32_t idx) {
    if (poolOff + 28 > a.size()) return std::string();
    if (idx >= rd32(&a[poolOff + 8])) return std::string();

    const size_t base = poolOff + rd32(&a[poolOff + 20]);
    const size_t offPos = poolOff + 28 + (size_t)idx * 4;
    if (offPos + 4 > a.size()) return std::string();

    size_t p = base + rd32(&a[offPos]);
    if (p >= a.size()) return std::string();

    if (utf8) {
        // UTF-8 池：UTF-16 字符数 + UTF-8 字节数 + 数据，两者各自可能是 1 或 2 字节
        size_t n = a[p++];
        if (n & 0x80) { if (p >= a.size()) return std::string(); p++; }
        if (p >= a.size()) return std::string();
        size_t len = a[p++];
        if (len & 0x80) { if (p >= a.size()) return std::string(); len = ((len & 0x7f) << 8) | a[p]; p++; }
        if (p + len > a.size()) return std::string();
        return std::string((const char*)&a[p], len);
    }

    if (p + 2 > a.size()) return std::string();
    size_t len = rd16(&a[p]);
    p += 2;
    if (p + len * 2 > a.size()) return std::string();

    std::string s;
    s.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        const uint16_t ch = rd16(&a[p + i * 2]);
        s.push_back(ch < 0x80 ? (char)ch : '?');   // versionName 恒为 ASCII
    }
    return s;
}

static inline std::string parseAxmlVersionName(const std::vector<uint8_t>& a) {
    if (a.size() < 8 || rd16(&a[0]) != 0x0003) return std::string();

    size_t poolOff = 0;
    bool poolUtf8 = false;
    bool havePool = false;

    size_t p = rd16(&a[2]);   // 跳过 XML 文件头
    while (p + 8 <= a.size()) {
        const uint16_t type = rd16(&a[p]);
        const uint32_t size = rd32(&a[p + 4]);
        if (size < 8 || p + size > a.size()) break;

        if (type == 0x0001) {                       // 字符串池
            poolOff = p;
            poolUtf8 = (rd32(&a[p + 16]) & 0x100) != 0;
            havePool = true;
        } else if (type == 0x0102 && havePool) {    // 开始标签
            const std::string elem = poolString(a, poolOff, poolUtf8, rd32(&a[p + 20]));

            if (elem == "manifest") {
                // attrStart 是相对 AttrExt 起点的偏移，而 AttrExt 紧跟在
                // 这个 chunk 的 16 字节 RES_XML_START_ELEMENT 头之后。
                // 用 chunk 起点当基准会整体错开 16 字节 —— 实测会读到
                // 命名空间 URI 而不是属性名。
                const size_t attrBase   = p + 16;
                const uint16_t attrStart = rd16(&a[p + 24]);
                const uint16_t attrSize  = rd16(&a[p + 26]);
                const uint16_t attrCount = rd16(&a[p + 28]);
                if (attrSize < 20) break;

                for (uint16_t i = 0; i < attrCount; ++i) {
                    const size_t ap = attrBase + attrStart + (size_t)i * attrSize;
                    if (ap + 20 > p + size || ap + 20 > a.size()) break;

                    if (poolString(a, poolOff, poolUtf8, rd32(&a[ap + 4])) != "versionName")
                        continue;

                    // rawValue 优先；为 -1 时退回 typed value 里的字符串索引
                    const uint32_t raw = rd32(&a[ap + 8]);
                    return poolString(a, poolOff, poolUtf8,
                                      raw != 0xFFFFFFFFu ? raw : rd32(&a[ap + 16]));
                }
                return std::string();
            }
        }
        p += size;
    }
    return std::string();
}

// ---------- 按需读 ZIP ----------

// 只读 EOCD 和中央目录，把 AndroidManifest.xml 的位置记下来
static inline bool locateManifest(FILE* f, uint64_t& localOff, uint32_t& compSize,
                                  uint32_t& uncompSize, uint16_t& method) {
    if (fseeko(f, 0, SEEK_END) != 0) return false;
    const uint64_t fileSize = (uint64_t)ftello(f);
    if (fileSize < 22) return false;

    // EOCD 在尾部，注释最长 65535，所以最多回看 22 + 65535 字节
    const uint64_t back = fileSize > 65557 ? 65557 : fileSize;
    std::vector<uint8_t> tail((size_t)back);
    if (!pread_at(f, fileSize - back, tail.data(), tail.size())) return false;

    int64_t eocd = -1;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (rd32(&tail[i]) == 0x06054b50u) { eocd = (int64_t)i; break; }
    }
    if (eocd < 0) return false;

    const uint16_t entries = rd16(&tail[eocd + 10]);
    const uint32_t cdSize  = rd32(&tail[eocd + 12]);
    const uint32_t cdOff   = rd32(&tail[eocd + 16]);

    if (cdSize == 0 || cdSize > (32u << 20)) return false;   // 中央目录不可能这么大
    if (cdOff + cdSize > fileSize) return false;

    std::vector<uint8_t> cd(cdSize);
    if (!pread_at(f, cdOff, cd.data(), cdSize)) return false;

    size_t p = 0;
    for (uint16_t n = 0; n < entries; ++n) {
        if (p + 46 > cd.size() || rd32(&cd[p]) != 0x02014b50u) return false;

        const uint16_t m        = rd16(&cd[p + 10]);
        const uint32_t csize    = rd32(&cd[p + 20]);
        const uint32_t usize    = rd32(&cd[p + 24]);
        const uint16_t nameLen  = rd16(&cd[p + 28]);
        const uint16_t extraLen = rd16(&cd[p + 30]);
        const uint16_t cmtLen   = rd16(&cd[p + 32]);
        const uint32_t lOff     = rd32(&cd[p + 42]);

        if (p + 46 + nameLen > cd.size()) return false;

        if (nameLen == 19 && memcmp(&cd[p + 46], "AndroidManifest.xml", 19) == 0) {
            localOff = lOff; compSize = csize; uncompSize = usize; method = m;
            return true;
        }
        p += 46u + nameLen + extraLen + cmtLen;
    }
    return false;
}

// 读出条目内容（stored 直接拷，deflate 用 zlib 解）
static inline bool readEntry(FILE* f, uint64_t localOff, uint32_t compSize,
                             uint32_t uncompSize, uint16_t method,
                             std::vector<uint8_t>& out) {
    uint8_t hdr[30];
    if (!pread_at(f, localOff, hdr, sizeof(hdr))) return false;
    if (rd32(hdr) != 0x04034b50u) return false;

    const size_t dataOff = (size_t)localOff + 30 + rd16(&hdr[26]) + rd16(&hdr[28]);
    if (compSize > (64u << 20)) return false;   // 清单不可能这么大

    std::vector<uint8_t> comp(compSize);
    if (!pread_at(f, dataOff, comp.data(), comp.size())) return false;

    if (method == 0) {
        out.swap(comp);
        return true;
    }
    if (method != 8) return false;

    // 解压缓冲由小到大试，不盲信 uncompSize（有些包会写 0）
    size_t cap = uncompSize ? uncompSize : (compSize * 8 + 4096);
    for (int attempt = 0; attempt < 5; ++attempt) {
        out.assign(cap, 0);

        z_stream zs;
        memset(&zs, 0, sizeof(zs));
        if (inflateInit2(&zs, -15) != Z_OK) return false;

        zs.next_in   = comp.data();
        zs.avail_in  = compSize;
        zs.next_out  = out.data();
        zs.avail_out = (uInt)out.size();

        const int rc = inflate(&zs, Z_FINISH);
        const size_t produced = out.size() - zs.avail_out;
        inflateEnd(&zs);

        if (rc == Z_STREAM_END) {
            out.resize(produced);
            return true;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) return false;

        // 缓冲区不够，翻倍再来
        cap *= 2;
        if (cap > (64u << 20)) return false;
    }
    return false;
}

// 从 /proc/self/maps 里找出 base.apk 的绝对路径
static inline std::string findApkPath() {
    FILE* fp = fopen("/proc/self/maps", "r");
    if (!fp) return std::string();

    char line[1024];
    std::string path;
    while (fgets(line, sizeof(line), fp)) {
        if (!strstr(line, "base.apk")) continue;
        const char* slash = strchr(line, '/');
        if (!slash) continue;
        path = slash;
        while (!path.empty() &&
               (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
            path.pop_back();
        break;
    }
    fclose(fp);
    return path;
}

// 失败返回空串。调用方应缓存结果 —— 解一次 APK 要几十毫秒。
static inline std::string getVersionName() {
    const std::string apk = findApkPath();
    if (apk.empty()) return std::string();

    FILE* f = fopen(apk.c_str(), "rb");
    if (!f) return std::string();

    std::string result;
    uint64_t localOff = 0;
    uint32_t compSize = 0, uncompSize = 0;
    uint16_t method = 0;

    if (locateManifest(f, localOff, compSize, uncompSize, method)) {
        std::vector<uint8_t> manifest;
        if (readEntry(f, localOff, compSize, uncompSize, method, manifest))
            result = parseAxmlVersionName(manifest);
    }
    fclose(f);
    return result;
}

} // namespace mcver
