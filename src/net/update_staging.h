// Persisted completion contract shared by download, restart recovery and apply.
#pragma once
#include <functional>
#include <set>
#include "net/update_check.h"

namespace rta::update {
struct ManifestFile { std::string path, sha256; long long size = 0; };

inline std::string norm_rel(std::string p) {
    for (char& c : p) if (c == '/') c = '\\';
    return p;
}
inline bool safe_install_rel(const std::string& p) {
    if (p.empty() || p.size() > 512 || p.front() == '/' || p.front() == '\\') return false;
    for (unsigned char c : p) if (c < 32 || c == 127 || c == ':' || c == '<' || c == '>' ||
        c == '"' || c == '|' || c == '?' || c == '*') return false;
    size_t start = 0;
    while (start < p.size()) {
        const size_t end = p.find_first_of("/\\", start);
        const std::string part = p.substr(start, end == std::string::npos ? end : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ') return false;
        std::string base = detail::lower(part.substr(0, part.find('.')));
        if (base == "con" || base == "prn" || base == "aux" || base == "nul" ||
            (base.size() == 4 && (base.substr(0, 3) == "com" || base.substr(0, 3) == "lpt") &&
             base[3] >= '1' && base[3] <= '9')) return false;
        if (end == std::string::npos) break;
        start = end + 1;
        if (start == p.size()) return false;
    }
    return true;
}
struct StagingFile { ManifestFile file; bool staged = false; };
struct StagingManifest { std::string version, installDir; std::vector<StagingFile> files; };
inline std::string stage_quote(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') { out += '\\'; out += (char)c; }
        else if (c < 32) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    return out + '"';
}
inline std::string serialize_staging_manifest(const StagingManifest& m) {
    std::string out = "{\"schema\":1,\"version\":" + stage_quote(m.version) +
        ",\"installDir\":" + stage_quote(m.installDir) + ",\"files\":[";
    for (size_t i = 0; i < m.files.size(); ++i) {
        const auto& e = m.files[i];
        if (i) out += ',';
        out += "{\"path\":" + stage_quote(e.file.path) + ",\"sha256\":" + stage_quote(e.file.sha256) +
            ",\"size\":" + std::to_string(e.file.size) + ",\"staged\":" + (e.staged ? "true" : "false") + '}';
    }
    return out + "]}";
}
inline bool parse_staging_manifest(const std::string& text, StagingManifest& out, std::string& err) {
    const auto j = rta::net::json_parse(text);
    if (j.type != rta::net::Json::Obj || j["schema"].type != rta::net::Json::Num || j["schema"].n() != 1 ||
        !parse_version(j["version"].s()).valid || j["installDir"].s().empty() ||
        j["files"].type != rta::net::Json::Arr || j["files"].arr.empty()) {
        err = "暂存完成清单格式非法"; return false;
    }
    StagingManifest m; m.version = j["version"].s(); m.installDir = j["installDir"].s();
    std::set<std::string> seen;
    for (const auto& e : j["files"].arr) {
        StagingFile f; f.file.path = e["path"].s(); f.file.sha256 = e["sha256"].s();
        const double size = e["size"].n();
        if (!safe_install_rel(f.file.path) || !is_hex64(f.file.sha256) ||
            e["size"].type != rta::net::Json::Num || size < 0 || size > 9007199254740991.0 ||
            size != std::floor(size) || e["staged"].type != rta::net::Json::Bool ||
            !seen.insert(detail::lower(norm_rel(f.file.path))).second) {
            err = "暂存完成清单文件条目非法或重复"; return false;
        }
        f.file.path = norm_rel(f.file.path); f.file.size = (long long)size; f.staged = e["staged"].b;
        m.files.push_back(f);
    }
    out = std::move(m); return true;
}
// The callback must check BOTH size and digest at the recorded origin. Reused install
// files are part of the full contract and must still match when apply is requested.
inline bool validate_staging_manifest(const StagingManifest& m, const std::string& version,
    const std::string& inst, const std::function<bool(const StagingFile&)>& check, std::string& err) {
    if (m.version != version || m.installDir != inst || m.files.empty()) {
        err = "暂存完成清单与版本/安装目录不匹配"; return false;
    }
    bool staged = false;
    for (const auto& e : m.files) {
        if (!check(e)) { err = "暂存完整性校验失败：" + e.file.path; return false; }
        staged = staged || e.staged;
    }
    if (!staged) { err = "暂存完成清单没有待安装文件"; return false; }
    return true;
}
} // namespace rta::update
