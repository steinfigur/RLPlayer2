// src/net/update.h — 在线更新（Windows 版）：清单检查 / 增量计划 / 下载 / 校验 / 解压 / 进度
//
// 需求（用户 261027-02 #5）：WIN 版也要在线更新；且（同轮追加）**要做增量更新，不做全量**。
// 用户原话：「在线功能的开发，不要做全量更新，尽量做成增量更新，考虑到我们项目实际不进入注册表，
//            所以完全可以做到自动更新，然后重启或者刷新，就可以完成。」
//
// 分层（照抄 MAC 那边的原则：所有判定在纯函数层，原生层只做 IO）：
//   · src/net/update_check.h —— MAC 移植过来的**纯逻辑层**：版本比较 / sha256 / 清单解析 /
//     URL 白名单 / 失败分支 / 配置解析。逐字未改（只换了清单文件名），167 条离线门禁照跑。
//   · 本文件 —— Windows 侧的 IO 与**增量**：拉清单、比对本地 sha256 得出「只下这几个文件」、
//     下载 delta zip（或逐文件兜底）、校验、解压到 staging、进度状态。
//   · src/net/update_apply.h —— 替换与回滚（启动器在 main() 最开头把 staging 落地）。
//
// 增量清单格式（在 MAC 的 latest.json 之上**只增不改**，老字段全部保留，MAC 客户端读它不受影响）：
//   {
//     "version": "1.6.0",
//     "download_url": "https://github.com/.../RLPlayer-1.6.0-delta.zip",  // 只含变化文件
//     "sha256": "<delta zip 的 sha256>",
//     "size": 12345678,
//     "notes": "...",
//     "published_at": "2026-10-27T...",
//     "files": [ {"path":"rtaweb.exe","sha256":"...","size":7449600}, ... ],   // 新版本的**全部**文件
//     "full": {"url":"https://github.com/.../RLPlayer-1.6.0-win64.zip","sha256":"...","size":390348760},
//     "files_base": "https://raw.githubusercontent.com/.../files/1.6.0/"      // 逐文件兜底前缀（可选）
//   }
// 客户端只下 files[] 里 sha256 与本机不一致的；delta zip 里缺的（跨版本、或本地文件被改过/删了）
// 走 files_base 逐文件兜底；再不行才回落到 full 全量包。
//
// 判据（全部可离线，见 tools/update_probe.cpp --selftest）：
//   1) 版本比较用 update_check.h 的语义化实现（1.5.9 < 1.5.10 < 1.6.0）
//   2) 增量计划：只把 sha256 不同的文件列进下载清单；改一个字节必须被识别
//   3) 校验：sha256 + size 双闸门，不过就丢弃（verify_downloaded_file）
//   4) 解压后逐文件再验一次 sha256（防 zip 内文件被换）
#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "version.h"
#include "net/http_client.h"
#include "net/json.h"
#include "net/update_staging.h"   // 完整暂存契约，复用 update_check.h 的版本/SHA 语义
#include "platform/http.h"        // onChunk + discardBody（边下边写盘）
#include "platform/path.h"
#include "platform/fs.h"
#include "platform/time.h"
#include "platform/update_support.h"
#include "platform/process.h"     // run_hidden（tar 解压）

namespace rta::update {

// ---------------------------------------------------------------------------
// 0. 版本 / 环境
// ---------------------------------------------------------------------------
inline std::string current_version() { return std::string(RLPLAYER_VERSION); }

// update_check.h 的 compare_versions 返回 enum，这里给一个 -1/0/1 的便捷包装（纯函数）
inline int cmp_versions(const std::string& a, const std::string& b) {
    const Version va = parse_version(a);
    const Version vb = parse_version(b);
    switch (compare_versions(va, vb)) {
        case VersionCmp::Less:    return -1;
        case VersionCmp::Greater: return 1;
        case VersionCmp::Equal:   return 0;
        default:                  return -1;
    }
}

// 安装目录 = exe 所在目录。环境变量 RLPLAYER_UPDATE_DIR 只用于**演练**（把整条链路
// 引到一个复制出来的测试目录），绝不会指向用户正在跑的目录 —— 除非有人显式设了它。
inline std::string install_dir() {
    if (const char* e = std::getenv("RLPLAYER_UPDATE_DIR")) {
        if (e && *e) return std::string(e);
    }
    static const std::string d = rta::platform::module_dir_utf8();
    return d;
}

// ---------------------------------------------------------------------------
// 1. 路径 / 文件小工具
// ---------------------------------------------------------------------------
inline std::string join_path(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}
inline bool path_exists(const std::string& p) {
    return rta::platform::file_exists(rta::platform::from_utf8(p));
}
inline bool is_dir(const std::string& p) {
    std::error_code ec;
    return std::filesystem::is_directory(rta::platform::from_utf8(p), ec);
}
inline long long file_size(const std::string& p) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(rta::platform::from_utf8(p), ec);
    return ec ? -1 : (long long)size;
}
inline bool ensure_dir(const std::string& p) {
    if (p.empty()) return false;
    if (is_dir(p)) return true;
    std::error_code ec;
    std::filesystem::create_directories(rta::platform::from_utf8(p), ec);
    return is_dir(p);
}
inline std::string read_text_file(const std::string& p) {
    std::ifstream f(rta::platform::from_utf8(p), std::ios::binary);
    if (!f) return std::string();
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}
inline bool write_text_file(const std::string& p, const std::string& text) {
    std::ofstream f(rta::platform::from_utf8(p), std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(text.data(), (std::streamsize)text.size());
    f.flush();
    if (!f) { f.close(); return false; }
    f.close();
    return !f.fail();
}
inline std::string lower_ascii(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}

inline bool rmtree(const std::string& dir) {
    const auto path = rta::platform::from_utf8(dir);
    if (!is_dir(dir)) {
        if (path_exists(dir)) {
            rta::platform::reset_file_attributes(path);
            return rta::platform::remove_file(path);
        }
        return true;
    }
    std::error_code ec;
    for (std::filesystem::directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        const auto child = it->path();
        if (it->is_directory(ec)) rmtree(rta::platform::to_utf8(child));
        else {
            rta::platform::reset_file_attributes(child);
            rta::platform::remove_file(child);
        }
    }
    return std::filesystem::remove(path, ec);
}

inline void list_files_rel(const std::string& root, const std::string& rel,
                           std::vector<std::string>& out) {
    const std::string dir = rel.empty() ? root : join_path(root, rel);
    std::error_code ec;
    for (std::filesystem::directory_iterator it(rta::platform::from_utf8(dir), ec), end;
         !ec && it != end; it.increment(ec)) {
        const std::string name = rta::platform::to_utf8(it->path().filename());
        const std::string childRel = rel.empty() ? name : rel + "\\" + name;
        if (it->is_directory(ec)) list_files_rel(root, childRel, out);
        else out.push_back(childRel);
    }
}

inline int count_files(const std::string& dir, int cap = 100000) {
    std::vector<std::string> v;
    list_files_rel(dir, "", v);
    return (int)(v.size() > (size_t)cap ? (size_t)cap : v.size());
}

inline std::string updates_root() { return join_path(install_dir(), "updates"); }
inline std::string version_dir(const std::string& v) { return join_path(updates_root(), v); }
inline std::string delta_name(const std::string& v) { return "RLPlayer-" + v + "-delta.zip"; }
inline std::string full_name(const std::string& v) { return "RLPlayer-" + v + "-win64.zip"; }

inline std::string tar_exe() { return rta::platform::system_tool_path("tar.exe"); }

// ---------------------------------------------------------------------------
// 2. 配置（rlplayer-update.conf）：跳过版本 / 清单地址
// ---------------------------------------------------------------------------
inline std::string conf_path() { return join_path(install_dir(), "rlplayer-update.conf"); }

inline std::map<std::string, std::string> conf_read() {
    std::map<std::string, std::string> kv;
    std::ifstream f(rta::platform::from_utf8(conf_path()));
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '#') continue;
        const size_t e = line.find('=');
        if (e == std::string::npos || e == 0) continue;
        kv[lower_ascii(line.substr(0, e))] = line.substr(e + 1);
    }
    return kv;
}
inline bool conf_write(const std::map<std::string, std::string>& kv) {
    std::string out;
    out += "# RLPlayer 在线更新配置（可删掉重来；token 留空 = 匿名读公开清单）\r\n";
    for (const auto& p : kv) out += p.first + "=" + p.second + "\r\n";
    return write_text_file(conf_path(), out);
}
inline std::string skip_version() {
    const auto kv = conf_read();
    auto it = kv.find("skip");
    return it == kv.end() ? std::string() : it->second;
}
inline void set_skip_version(const std::string& v) {
    auto kv = conf_read();
    if (v.empty()) kv.erase("skip"); else kv["skip"] = v;
    conf_write(kv);
}

// 清单/下载地址的白名单：
//   生产路径**只认** update_check.h 里写死的两个主机（raw.githubusercontent.com / github.com）。
//   环境变量 RLPLAYER_UPDATE_ALLOW_LOCAL=1 时才额外放行本机 http（演练/离线自检用）——
//   它是显式的测试开关，默认关闭，且只放行 127.0.0.1/localhost。
inline bool allow_local_urls() {
    const char* e = std::getenv("RLPLAYER_UPDATE_ALLOW_LOCAL");
    return e && *e && std::strcmp(e, "0") != 0;
}
inline bool url_allowed(const std::string& url) {
    if (update_download_url_allowed(url)) return true;
    if (!allow_local_urls()) return false;
    if (url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://localhost", 0) == 0)
        return url.size() > 20;
    return false;
}

// ---------------------------------------------------------------------------
// 3. 清单扩展（files[] / full / files_base）——纯函数，可离线断言
// ---------------------------------------------------------------------------
struct ManifestExt {
    bool ok = false;
    std::string error;
    std::string version;
    std::vector<ManifestFile> files;
    std::string deltaUrl, deltaSha;
    long long deltaSize = 0;
    std::string fullUrl, fullSha;
    long long fullSize = 0;
    std::string filesBase;
    std::string notes;
    std::string publishedAt;
};

inline ManifestExt parse_manifest_ext(const std::string& json) {
    ManifestExt m;
    const rta::net::Json j = rta::net::json_parse(json);
    if (j.type != rta::net::Json::Obj) { m.error = "清单不是 JSON 对象"; return m; }
    m.version = j["version"].s();
    if (!parse_version(m.version).valid || m.version.find_first_of("/\\\r\n") != std::string::npos) { m.error = "清单 version 非法"; return m; }
    m.notes = j["notes"].s();
    m.publishedAt = j["published_at"].s();
    const std::string dl = j["download_url"].s();
    if (url_allowed(dl)) {
        m.deltaUrl = dl;
        m.deltaSha = lower_ascii(j["sha256"].s());
        if (j.has("size") && !parse_manifest_size(j["size"], m.deltaSize)) {
            m.error = "清单下载包 size 非法"; return m;
        }
    }
    const rta::net::Json& full = j["full"];
    if (full.type == rta::net::Json::Obj) {
        const std::string fu = full["url"].s();
        if (url_allowed(fu)) {
            m.fullUrl = fu;
            m.fullSha = lower_ascii(full["sha256"].s());
            if (full.has("size") && !parse_manifest_size(full["size"], m.fullSize)) {
                m.error = "清单 full.size 非法"; return m;
            }
        }
    }
    const std::string fb = j["files_base"].s();
    if (!fb.empty() && url_allowed(fb)) m.filesBase = fb;

    std::set<std::string> seen;
    const rta::net::Json& arr = j["files"];
    if (arr.type == rta::net::Json::Arr) {
        for (size_t i = 0; i < arr.size(); ++i) {
            const rta::net::Json& e = arr.at(i);
            const std::string rawPath = e["path"].s();
            // ★ 先按**原始串**判断穿越（归一化会把 "../" 削成 ""，那就永远查不出来）
            if (!safe_install_rel(rawPath)) {
                m.error = "清单 files[] 第 " + std::to_string(i + 1) + " 条路径不安全（穿越/绝对路径）";
                return m;
            }
            ManifestFile f;
            f.path = norm_rel(rawPath);
            f.sha256 = lower_ascii(e["sha256"].s());
            if (!parse_manifest_size(e["size"], f.size)) { m.error = "清单文件 size 非法"; return m; }
            if (!safe_install_rel(f.path) || !is_hex64(f.sha256) || !seen.insert(lower_ascii(f.path)).second) {
                m.error = "清单 files[] 第 " + std::to_string(i + 1) + " 条不合法（路径或 sha256）";
                return m;
            }
            m.files.push_back(f);
        }
    }
    if (m.files.empty()) { m.error = "清单缺少 files[]（增量更新需要逐文件 sha256）"; return m; }
    m.ok = true;
    return m;
}

inline std::recursive_mutex& update_artifacts_mutex() { static std::recursive_mutex mutex; return mutex; }
// One writer per installation, including other launcher/rtaweb processes. The local
// recursive lock gives each same-thread nested apply->rollback scope the same lease.
class UpdateWriterLease {
public:
    explicit UpdateWriterLease(std::string& err)
        : local_(update_artifacts_mutex(), std::try_to_lock) {
        if (!local_.owns_lock()) { err = "更新文件正在使用，请稍后重试"; return; }
        auto& held = current();
        const std::string path = join_path(updates_root(), "writer.lock");
        if (held.depth != 0) {
            if (held.path != path) { err = "嵌套更新事务的安装目录不一致"; return; }
        } else {
            if (!ensure_dir(updates_root())) { err = "无法创建更新写锁目录"; return; }
            held.path = path;
            held.lease = std::make_unique<rta::platform::FileLease>(rta::platform::from_utf8(path),
                rta::platform::FileLeaseMode::WriterExclusive);
            if (!held.lease->ok()) {
                const int code = held.lease->error_code();
                const bool busy = held.lease->contended();
                held.path.clear(); held.lease.reset();
                err = busy ? "另一进程正在使用更新文件，请稍后重试"
                    : "无法获取更新写锁（Windows 错误 " + std::to_string(code) + "）";
                return;
            }
        }
        ++held.depth; owns_ = true;
    }
    ~UpdateWriterLease() {
        if (!owns_) return;
        auto& held = current();
        if (--held.depth == 0) { held.lease.reset(); held.path.clear(); }
    }
    bool owns_lock() const { return owns_; }
    UpdateWriterLease(const UpdateWriterLease&) = delete;
    UpdateWriterLease& operator=(const UpdateWriterLease&) = delete;
private:
    struct Held { std::string path; std::unique_ptr<rta::platform::FileLease> lease; unsigned depth = 0; };
    static Held& current() { static thread_local Held held; return held; }
    std::unique_lock<std::recursive_mutex> local_;
    bool owns_ = false;
};


// The digest marker is written last and atomically: no directory enumeration can
// turn an interrupted download into a completed update.
inline std::string staging_manifest_path(const std::string& ver) { return join_path(version_dir(ver), "staging-manifest.json"); }
inline std::string staging_complete_path(const std::string& ver) { return join_path(version_dir(ver), "staging-complete.sha256"); }
inline bool write_update_atomic(const std::string& path, const std::string& text) {
    const std::string tmp = path + ".tmp";
    if (!write_text_file(tmp, text)) return false;
    return rta::platform::durable_replace_file(rta::platform::from_utf8(tmp), rta::platform::from_utf8(path));
}
inline bool verify_staging(const std::string& ver, const std::string& staging,
    StagingManifest& m, std::string& err) {
    if (!parse_version(ver).valid || ver.find_first_of("/\\\r\n") != std::string::npos ||
        staging != join_path(version_dir(ver), "staging")) { err = "暂存路径与版本不匹配"; return false; }
    const std::string text = read_text_file(staging_manifest_path(ver));
    if (read_text_file(staging_complete_path(ver)) != sha256_hex(text)) {
        err = "暂存未完成或清单摘要不符"; return false;
    }
    if (!parse_staging_manifest(text, m, err)) return false;
    return validate_staging_manifest(m, ver, install_dir(), [&](const StagingFile& e) {
        const std::string path = join_path(e.staged ? staging : install_dir(), e.file.path);
        std::string hex, why;
        return file_size(path) == e.file.size && sha256_file(path, hex, why) && hex_equals_ci(hex, e.file.sha256);
    }, err);
}
inline bool complete_staging(const ManifestExt& man, const std::vector<std::string>& downloads,
    const std::string& staging, std::string& err) {
    StagingManifest m; m.version = man.version; m.installDir = install_dir();
    std::set<std::string> staged;
    for (const auto& p : downloads) staged.insert(lower_ascii(norm_rel(p)));
    for (const auto& f : man.files) m.files.push_back({f, staged.count(lower_ascii(norm_rel(f.path))) != 0});
    const std::string text = serialize_staging_manifest(m);
    StagingManifest checked;
    if (!parse_staging_manifest(text, checked, err) || !validate_staging_manifest(checked, man.version,
        install_dir(), [&](const StagingFile& e) {
            const std::string path = join_path(e.staged ? staging : install_dir(), e.file.path);
            std::string hex, why;
            return file_size(path) == e.file.size && sha256_file(path, hex, why) && hex_equals_ci(hex, e.file.sha256);
        }, err)) return false;
    if (!write_update_atomic(staging_manifest_path(man.version), text) ||
        !write_update_atomic(staging_complete_path(man.version), sha256_hex(text))) {
        err = "无法持久化暂存完成清单"; return false;
    }
    return verify_staging(man.version, staging, m, err);
}

// ---------------------------------------------------------------------------
// 4. 增量计划：只下载本机 sha256 与清单不一致的文件
// ---------------------------------------------------------------------------
struct PlanItem {
    std::string path;
    std::string wantSha;
    std::string haveSha;
    long long wantSize = 0;
    bool same = false;
};

struct Plan {
    bool ok = false;
    std::string error;
    std::vector<PlanItem> items;
    int toDownload = 0, unchanged = 0;
    long long downloadBytes = 0, skippedBytes = 0;
    std::vector<std::string> missing;
};

inline Plan plan_incremental(const std::vector<ManifestFile>& files, const std::string& dir) {
    Plan p;
    if (files.empty()) { p.error = "清单里没有文件"; return p; }
    for (const auto& f : files) {
        PlanItem it;
        it.path = f.path;
        it.wantSha = f.sha256;
        it.wantSize = f.size;
        const std::string target = join_path(dir, f.path);
        std::string hex, err;
        if (!sha256_file(target, hex, err)) {
            it.same = false;
        } else {
            it.haveSha = hex;
            it.same = hex_equals_ci(hex, f.sha256);
        }
        if (it.same) { ++p.unchanged; p.skippedBytes += (f.size > 0 ? f.size : 0); }
        else { ++p.toDownload; p.downloadBytes += (f.size > 0 ? f.size : 0); p.missing.push_back(f.path); }
        p.items.push_back(it);
    }
    p.ok = true;
    return p;
}

// ---------------------------------------------------------------------------
// 5. JSON 输出
// ---------------------------------------------------------------------------
inline std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string size_text(long long n) {
    if (n <= 0) return std::string("—");
    char b[64];
    if (n >= 1024LL * 1024 * 1024) std::snprintf(b, sizeof(b), "%.2f GB", (double)n / (1024.0 * 1024 * 1024));
    else if (n >= 1024LL * 1024)    std::snprintf(b, sizeof(b), "%.1f MB", (double)n / (1024.0 * 1024));
    else                            std::snprintf(b, sizeof(b), "%.1f KB", (double)n / 1024.0);
    return std::string(b);
}

// ---------------------------------------------------------------------------
// 6. 运行时状态（后台线程 + 锁；WebUi 是单线程 accept，端点只投递/读状态）
// ---------------------------------------------------------------------------
struct UpdateState {
    std::mutex mtx;
    bool checking = false;
    bool hasChecked = false;
    unsigned long long checkedMs = 0;
    CheckResult cr;
    ManifestExt man;
    Plan plan;
    std::string manifestUrl;
    std::string checkError;
    std::string planJson;
    bool downloading = false;
    std::string phase = "idle";
    long long downloaded = 0, total = 0;
    double speedBps = 0.0;
    std::string dlError;
    std::string dlVersion;
    bool staged = false;
    int stagedFiles = 0;
    std::string shaActual, shaExpected;
    unsigned long long dlStartMs = 0, dlEndMs = 0;
    std::string stagingDir;
    int perFileFetches = 0;
};

inline UpdateState& state() { static UpdateState s; return s; }
inline unsigned long long now_ms() { return rta::platform::monotonic_ms(); }
inline constexpr unsigned long long kCheckTtlMs = 300ull * 1000ull;

inline std::string manifest_url() {
    if (const char* e = std::getenv("RLPLAYER_UPDATE_MANIFEST")) {
        if (e && *e && url_allowed(e)) return std::string(e);
    }
    const auto kv = conf_read();
    auto it = kv.find("manifest_url");
    if (it != kv.end() && !it->second.empty() && url_allowed(it->second)) return it->second;
    return std::string(kDefaultManifestUrl);
}

inline bool fetch_and_evaluate(int timeoutMs, std::string& err) {
    if (timeoutMs <= 0) timeoutMs = 8000;
    if (timeoutMs > 10000) timeoutMs = 10000;

    const std::string cur = current_version();
    const std::string url = manifest_url();
    std::vector<std::string> hdr = {"User-Agent: RLPlayer/" + cur};
    {
        const Config cfg = parse_update_config(read_text_file(conf_path()));
        if (cfg.hasToken) hdr.push_back("Authorization: Bearer " + cfg.token);
    }
    auto resp = rta::net::http_get(url, hdr, timeoutMs);
    const std::string body(resp.body.begin(), resp.body.end());

    // ★ 生产路径的唯一判定入口：HTTP 结果 + 本地版本交给纯逻辑层（update_check.h）
    CheckResult cr;
    if (allow_local_urls()) {
        // 演练旁路（RLPLAYER_UPDATE_ALLOW_LOCAL=1，默认关闭）：纯逻辑层只认
        // raw.githubusercontent.com / github.com 两个主机，本机 http 清单必然被判 FailedBadUrl。
        // 这里用本文件的宽松解析（也只放行 127.0.0.1/localhost）走同一条决策语义，
        // 让"清单 → 增量计划 → 下载 → 校验 → 落地 → 回滚"能在离线环境里端到端演练。
        if (!resp.ok) {
            cr.decision = (resp.status == 0) ? Decision::FailedNetwork : Decision::FailedHttp;
            cr.status = resp.status;
            cr.error = (resp.status == 0) ? "网络不可用（无法连接更新服务器）"
                                          : ("更新服务器返回 HTTP " + std::to_string(resp.status));
        } else {
            const ManifestExt m0 = parse_manifest_ext(body);
            cr.status = resp.status;
            if (!m0.ok) { cr.decision = Decision::FailedParse; cr.error = m0.error; }
            else {
                cr.decision = (cmp_versions(m0.version, cur) > 0) ? Decision::UpdateAvailable
                                                                 : Decision::UpToDate;
                cr.remoteVersion = m0.version;
                cr.downloadUrl = m0.deltaUrl;
                cr.sha256 = m0.deltaSha;
                cr.size = m0.deltaSize;
                cr.notes = m0.notes;
            }
        }
    } else {
        cr = evaluate_update_check(cur, resp.ok, resp.status, body);
    }

    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.cr = cr;
        s.manifestUrl = url;
        s.man = ManifestExt{};
        s.plan = Plan{};
        s.planJson.clear();
        s.checkError = cr.ok() ? std::string() : cr.error;
    }
    if (!cr.ok()) { err = cr.error; return false; }
    if (cr.decision != Decision::UpdateAvailable) return true;

    ManifestExt m = parse_manifest_ext(body);
    if (!m.ok) {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.checkError = "清单解析失败：" + m.error;
        s.cr.decision = Decision::FailedParse;
        s.cr.error = s.checkError;
        err = s.checkError;
        return false;
    }
    Plan pl = plan_incremental(m.files, install_dir());
    if (!pl.ok) {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.checkError = "增量计划生成失败：" + pl.error;
        err = s.checkError;
        return false;
    }

    std::string pj = "[";
    int n = 0;
    for (const auto& it : pl.items) {
        if (it.same) continue;
        if (n++) pj += ",";
        if (n > 60) { pj += "\"...\""; break; }
        pj += "{\"path\":\"" + json_escape(it.path) + "\",\"size\":" + std::to_string(it.wantSize) +
              ",\"have\":\"" + json_escape(it.haveSha) + "\"}";
    }
    pj += "]";

    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.man = m;
        s.plan = pl;
        s.planJson = pj;
        if (!skip_version().empty() && cmp_versions(skip_version(), m.version) == 0) {
            s.cr.decision = Decision::UpToDate;
            s.cr.error.clear();
        }
    }
    return true;
}

inline std::string check_json() {
    auto& s = state();
    CheckResult cr;
    Plan pl;
    ManifestExt man;
    std::string err, url, pj;
    bool checking = false, hasChecked = false;
    unsigned long long at = 0;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        cr = s.cr; pl = s.plan; man = s.man; err = s.checkError; url = s.manifestUrl;
        checking = s.checking; hasChecked = s.hasChecked; at = s.checkedMs; pj = s.planJson;
    }
    const long long ageSec = hasChecked ? (long long)((now_ms() - at) / 1000) : -1;
    std::string j = "{";
    j += "\"ok\":" + std::string(cr.ok() ? "true" : "false");
    j += ",\"decision\":\"" + std::string(cr.code()) + "\"";
    j += ",\"current\":\"" + json_escape(current_version()) + "\"";
    j += ",\"latest\":\"" + json_escape(cr.remoteVersion) + "\"";
    j += ",\"hasUpdate\":" + std::string(cr.decision == Decision::UpdateAvailable ? "true" : "false");
    j += ",\"upToDate\":" + std::string(cr.decision == Decision::UpToDate ? "true" : "false");
    j += ",\"error\":\"" + json_escape(err) + "\"";
    j += ",\"notes\":\"" + json_escape(cr.notes) + "\"";
    j += ",\"publishedAt\":\"" + json_escape(man.publishedAt) + "\"";
    j += ",\"manifestUrl\":\"" + json_escape(url) + "\"";
    j += ",\"checking\":" + std::string(checking ? "true" : "false");
    j += ",\"checked\":" + std::string(hasChecked ? "true" : "false");
    j += ",\"ageSec\":" + std::to_string(ageSec);
    j += ",\"cached\":" + std::string(hasChecked && !checking && ageSec >= 0 && ageSec < 300 ? "true" : "false");
    j += ",\"skipVersion\":\"" + json_escape(skip_version()) + "\"";
    j += ",\"filesTotal\":" + std::to_string((long long)man.files.size());
    j += ",\"filesToDownload\":" + std::to_string(pl.toDownload);
    j += ",\"filesUnchanged\":" + std::to_string(pl.unchanged);
    j += ",\"downloadBytes\":" + std::to_string(pl.downloadBytes);
    j += ",\"skippedBytes\":" + std::to_string(pl.skippedBytes);
    j += ",\"downloadText\":\"" + json_escape(size_text(pl.downloadBytes)) + "\"";
    j += ",\"skippedText\":\"" + json_escape(size_text(pl.skippedBytes)) + "\"";
    j += ",\"fullBytes\":" + std::to_string(man.fullSize);
    j += ",\"deltaBytes\":" + std::to_string(man.deltaSize);
    j += ",\"deltaUrl\":\"" + json_escape(man.deltaUrl) + "\"";
    // 与需求原文的字段名对齐（url / sizeBytes / prerelease）：UI 与外部脚本两套名字都能读
    j += ",\"url\":\"" + json_escape(man.deltaUrl.empty() ? man.fullUrl : man.deltaUrl) + "\"";
    j += ",\"sizeBytes\":" + std::to_string(man.deltaSize > 0 ? man.deltaSize : man.fullSize);
    j += ",\"prerelease\":" + std::string(parse_version(cr.remoteVersion).has_pre() ? "true" : "false");
    j += ",\"files\":" + (pj.empty() ? std::string("[]") : pj);
    j += ",\"installDir\":\"" + json_escape(install_dir()) + "\"";
    j += "}";
    return j;
}

inline void check_worker() {
    std::string err;
    fetch_and_evaluate(8000, err);
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.hasChecked = true;
    s.checkedMs = now_ms();
    s.checking = false;
    if (s.phase == "checking") s.phase = "idle";
}

inline bool start_check(bool force) {
    auto& s = state();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.checking) return true;
        if (!force && s.hasChecked && s.cr.ok() && now_ms() - s.checkedMs < kCheckTtlMs) return true;
        s.checking = true;
        if (s.phase == "idle") s.phase = "checking";
    }
    std::thread(check_worker).detach();
    return true;
}

inline bool check_now(int timeoutMs) {
    auto& s = state();
    { std::lock_guard<std::mutex> lk(s.mtx); s.checking = true; }
    std::string err;
    const bool ok = fetch_and_evaluate(timeoutMs, err);
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.hasChecked = true;
        s.checkedMs = now_ms();
        s.checking = false;
        if (!ok && s.checkError.empty()) s.checkError = err;
    }
    return ok;
}

// ---------------------------------------------------------------------------
// 7. 下载 + 校验 + 解压
// ---------------------------------------------------------------------------
inline bool http_download(const std::string& url, const std::string& dst, int timeoutMs,
                          long long& outBytes, std::string& outSha, std::string& err) {
    rta::platform::remove_file(rta::platform::from_utf8(dst));
    std::ofstream out(rta::platform::from_utf8(dst), std::ios::binary | std::ios::trunc);
    if (!out) { err = "无法写入：" + dst; return false; }
    bool writeErr = false;
    long long bytes = 0;
    unsigned long long lastMs = now_ms();
    long long lastBytes = 0;
    auto& s = state();

    rta::platform::HttpRequest req;
    req.method      = "GET";
    req.url         = url;
    req.timeoutMs   = timeoutMs;
    req.discardBody = true;
    req.headers     = {"User-Agent: RLPlayer/" + current_version()};
    req.onChunk     = [&](const char* d, size_t n) {
        out.write(d, (std::streamsize)n);
        if (!out) { writeErr = true; return; }
        bytes += (long long)n;
        const unsigned long long t = now_ms();
        std::lock_guard<std::mutex> lk(s.mtx);
        s.downloaded += (long long)n;
        if (t - lastMs >= 300) {
            const double dt = (double)(t - lastMs) / 1000.0;
            if (dt > 0.05) s.speedBps = (double)(s.downloaded - lastBytes) / dt;
            lastMs = t; lastBytes = s.downloaded;
        }
    };
    const auto resp = rta::platform::http_request(req);
    out.close();
    if (writeErr) { rta::platform::remove_file(rta::platform::from_utf8(dst)); err = "写盘失败（磁盘满/无权限）：" + dst; return false; }
    if (!resp.ok) {
        rta::platform::remove_file(rta::platform::from_utf8(dst));
        err = (resp.status == 0) ? (resp.error.empty() ? "网络不可达" : resp.error)
                                 : ("HTTP " + std::to_string(resp.status));
        return false;
    }
    outBytes = bytes;
    std::string e2;
    if (!sha256_file(dst, outSha, e2)) { err = e2.empty() ? "读取下载文件失败" : e2; return false; }
    return true;
}

inline bool unzip_to(const std::string& zip, const std::string& dst, std::string& err) {
    rmtree(dst);
    if (!ensure_dir(dst)) { err = "无法创建目录：" + dst; return false; }
    const std::string cmd = "\"" + tar_exe() + "\" -xf \"" + zip + "\" -C \"" + dst + "\"";
    auto pr = rta::platform::run_hidden(cmd, true);
    if (!pr.started) { err = "无法启动 tar.exe"; return false; }
    if (pr.exit_code != 0) {
        err = "解压失败（tar 退出码 " + std::to_string(pr.exit_code) + "）" +
              (pr.output.empty() ? std::string() : "：" + pr.output.substr(0, 200));
        return false;
    }
    return true;
}

inline void download_worker(ManifestExt man, Plan pl) {
    auto& s = state();
    const std::string ver = man.version;
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.phase = "error";
        s.dlError = why;
        s.downloading = false;
        s.staged = false;
        s.dlEndMs = now_ms();
    };
    std::string leaseError;
    UpdateWriterLease writer(leaseError);
    if (!writer.owns_lock()) { fail(leaseError); return; }
    auto set_phase = [&](const char* p) {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.phase = p;
    };

    const std::string dir = version_dir(ver);
    const std::string staging = join_path(dir, "staging");
    const std::string unpack = join_path(dir, "unpacked");
    if (!ensure_dir(dir)) { fail("无法创建目录：" + dir + "（安装目录不可写？）"); return; }
    if (path_exists(staging_complete_path(ver)) && !rta::platform::remove_file(rta::platform::from_utf8(staging_complete_path(ver)))) { fail("无法清除旧暂存完成标记"); return; }
    if (!rmtree(staging)) { fail("无法清理旧 staging"); return; }
    if (!ensure_dir(staging)) { fail("无法创建 staging：" + staging); return; }

    std::map<std::string, std::string> want;
    for (const auto& f : man.files) want[lower_ascii(f.path)] = f.sha256;
    std::vector<std::string> pending = pl.missing;

    auto take_from = [&](const std::string& base) {
        std::vector<std::string> left;
        for (const auto& rel : pending) {
            const std::string src = join_path(base, rel);
            const std::string dst = join_path(staging, rel);
            if (!path_exists(src)) { left.push_back(rel); continue; }
            std::string hex, e2;
            if (!sha256_file(src, hex, e2)) { left.push_back(rel); continue; }
            const std::string w = want[lower_ascii(rel)];
            if (!hex_equals_ci(hex, w)) { left.push_back(rel); continue; }
            const size_t sp = dst.find_last_of('\\');
            if (sp != std::string::npos) ensure_dir(dst.substr(0, sp));
            rta::platform::remove_file(rta::platform::from_utf8(dst));
            if (!rta::platform::move_file(rta::platform::from_utf8(src), rta::platform::from_utf8(dst), true))
                left.push_back(rel);
        }
        pending.swap(left);
    };

    // ① delta zip
    if (!man.deltaUrl.empty() && !pending.empty()) {
        const std::string zip = join_path(dir, delta_name(ver));
        long long bytes = 0;
        std::string sha, err;
        if (!http_download(man.deltaUrl, zip, 30000, bytes, sha, err)) { fail("下载 delta 失败：" + err); return; }
        set_phase("verifying");
        {
            std::lock_guard<std::mutex> lk(s.mtx);
            s.shaActual = sha;
            s.shaExpected = man.deltaSha;
        }
        const VerifyResult vr = verify_downloaded_file(zip, man.deltaSha, man.deltaSize);
        if (!vr.ok) { rta::platform::remove_file(rta::platform::from_utf8(zip)); fail("delta 校验失败：" + vr.error); return; }
        set_phase("extracting");
        if (!unzip_to(zip, unpack, err)) { rta::platform::remove_file(rta::platform::from_utf8(zip)); fail(err); return; }
        take_from(unpack);
        rmtree(unpack);
        rta::platform::remove_file(rta::platform::from_utf8(zip));
    }

    // ② 逐文件兜底
    if (!pending.empty() && !man.filesBase.empty()) {
        set_phase("downloading-one-by-one");
        std::vector<std::string> left;
        int fetched = 0;
        std::string base = man.filesBase;
        if (!base.empty() && base.back() != '/') base += "/";
        for (const auto& rel : pending) {
            std::string relUrl = rel;
            for (char& c : relUrl) if (c == '\\') c = '/';
            const std::string url = base + relUrl;
            const std::string tmp = join_path(dir, "one.bin");
            long long bytes = 0;
            std::string sha, err;
            if (!http_download(url, tmp, 30000, bytes, sha, err)) { left.push_back(rel); continue; }
            const std::string w = want[lower_ascii(rel)];
            if (!hex_equals_ci(sha, w)) { left.push_back(rel); rta::platform::remove_file(rta::platform::from_utf8(tmp)); continue; }
            const std::string dst = join_path(staging, rel);
            const size_t sp = dst.find_last_of('\\');
            if (sp != std::string::npos) ensure_dir(dst.substr(0, sp));
            rta::platform::remove_file(rta::platform::from_utf8(dst));
            if (rta::platform::move_file(rta::platform::from_utf8(tmp), rta::platform::from_utf8(dst), true)) ++fetched;
            else left.push_back(rel);
        }
        pending.swap(left);
        std::lock_guard<std::mutex> lk(s.mtx);
        s.perFileFetches = fetched;
    }

    // ③ 全量兜底
    if (!pending.empty() && !man.fullUrl.empty()) {
        set_phase("downloading-full");
        const std::string zip = join_path(dir, full_name(ver));
        long long bytes = 0;
        std::string sha, err;
        if (http_download(man.fullUrl, zip, 30000, bytes, sha, err)) {
            const VerifyResult vr = verify_downloaded_file(zip, man.fullSha, man.fullSize);
            if (vr.ok && unzip_to(zip, unpack, err)) {
                take_from(unpack);
                rmtree(unpack);
            }
            rta::platform::remove_file(rta::platform::from_utf8(zip));
        }
    }

    if (!pending.empty()) {
        std::string missing;
        for (size_t i = 0; i < pending.size() && i < 6; ++i) missing += (i ? ", " : "") + pending[i];
        fail("有 " + std::to_string(pending.size()) + " 个文件拿不到（" + missing + "…）");
        return;
    }

    std::string completeError;
    if (!complete_staging(man, pl.missing, staging, completeError)) { fail(completeError); return; }
    const int files = (int)pl.missing.size();
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.phase = "ready";
        s.downloading = false;
        s.staged = true;
        s.stagedFiles = files;
        s.dlVersion = ver;
        s.stagingDir = staging;
        s.dlEndMs = now_ms();
    }
}

inline bool start_download(std::string& err) {
    ManifestExt downloadManifest; Plan downloadPlan;
    auto& s = state();
    {
        UpdateWriterLease writer(err);
        if (!writer.owns_lock()) return false;
        std::lock_guard<std::mutex> lk(s.mtx);
        if (s.downloading) return true;
        if (s.cr.decision != Decision::UpdateAvailable) {
            err = (s.cr.decision == Decision::UpToDate) ? "已经是最新版本，无需下载" : "请先检查更新";
            return false;
        }
        if (s.plan.toDownload == 0) { err = "没有需要下载的文件（本地已与清单一致）"; return false; }
        if (s.man.deltaUrl.empty() && s.man.filesBase.empty() && s.man.fullUrl.empty()) {
            err = "清单里没有任何可用下载地址";
            return false;
        }
        downloadManifest = s.man; downloadPlan = s.plan;
        s.downloading = true;
        s.phase = "downloading";
        s.downloaded = 0;
        // 进度分母优先用清单声明的**传输大小**（压缩后的 delta/全量包）；
        // plan.downloadBytes 是解压后的大小，拿它当分母会让进度超过 100%。
        s.total = s.man.deltaSize > 0 ? s.man.deltaSize : s.plan.downloadBytes;
        s.speedBps = 0;
        s.dlError.clear();
        s.dlVersion = s.man.version;
        s.staged = false;
        s.stagedFiles = 0;
        s.shaActual.clear();
        s.shaExpected = s.man.deltaSha;
        s.dlStartMs = now_ms();
        s.dlEndMs = 0;
        s.perFileFetches = 0;
        s.stagingDir = join_path(version_dir(s.man.version), "staging");
    }
    try { std::thread(download_worker, std::move(downloadManifest), std::move(downloadPlan)).detach(); }
    catch (...) {
        std::lock_guard<std::mutex> lock(s.mtx); s.downloading = false; s.phase = "error";
        s.dlError = err = "无法启动更新下载线程"; return false;
    }
    return true;
}

inline std::string progress_json() {
    auto& s = state();
    std::string phase, e, ver, shaA, shaE, staging;
    long long dl = 0, total = 0;
    double sp = 0;
    bool staged = false;
    int files = 0, perFile = 0;
    unsigned long long t0 = 0, t1 = 0;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        phase = s.phase; e = s.dlError; ver = s.dlVersion;
        dl = s.downloaded; total = s.total; sp = s.speedBps;
        staged = s.staged; files = s.stagedFiles;
        shaA = s.shaActual; shaE = s.shaExpected;
        t0 = s.dlStartMs; t1 = s.dlEndMs; perFile = s.perFileFetches;
        staging = s.stagingDir;
    }
    const double pct = (total > 0) ? (100.0 * (double)dl / (double)total) : 0.0;
    const unsigned long long elapsed = (t1 > t0) ? (t1 - t0) : (t0 > 0 ? (now_ms() - t0) : 0);
    std::string j = "{";
    j += "\"ok\":true";
    j += ",\"phase\":\"" + json_escape(phase) + "\"";
    j += ",\"version\":\"" + json_escape(ver) + "\"";
    j += ",\"downloaded\":" + std::to_string(dl);
    j += ",\"total\":" + std::to_string(total);
    j += ",\"pct\":" + std::to_string((int)(pct * 10.0) / 10.0);
    j += ",\"speed\":" + std::to_string((long long)sp);
    j += ",\"speedText\":\"" + json_escape(sp > 0 ? size_text((long long)sp) + "/s" : std::string("—")) + "\"";
    j += ",\"elapsedMs\":" + std::to_string(elapsed);
    j += ",\"staged\":" + std::string(staged ? "true" : "false");
    j += ",\"files\":" + std::to_string(files);
    j += ",\"perFileFetches\":" + std::to_string(perFile);
    j += ",\"sha256\":\"" + json_escape(shaA) + "\"";
    j += ",\"expectedSha256\":\"" + json_escape(shaE) + "\"";
    j += ",\"stagingDir\":\"" + json_escape(staging) + "\"";
    j += ",\"error\":\"" + json_escape(e) + "\"";
    j += ",\"canApply\":" + std::string(staged && phase == "ready" ? "true" : "false");
    j += "}";
    return j;
}

inline void restore_staging_state() {
    auto& s = state();
    std::lock_guard<std::mutex> lk(s.mtx);
    if (s.staged || s.downloading) return;
    std::string best;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(rta::platform::from_utf8(updates_root()), ec), end;
         !ec && it != end; it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        const std::string v = rta::platform::to_utf8(it->path().filename());
        const std::string st = join_path(join_path(updates_root(), v), "staging");
        StagingManifest manifest; std::string error;
        if (is_dir(st) && verify_staging(v, st, manifest, error)) {
            if (best.empty() || cmp_versions(v, best) > 0) best = v;
        }
    }
    if (best.empty()) return;
    const std::string st = join_path(version_dir(best), "staging");
    s.staged = true;
    s.stagedFiles = count_files(st);
    s.dlVersion = best;
    s.stagingDir = st;
    s.phase = "ready";
}

} // namespace rta::update
