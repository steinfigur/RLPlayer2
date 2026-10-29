// Checked publication of a newly allocated cache object. Never replaces an existing object.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#ifdef _WIN32
#include "platform/fs.h"
#endif

namespace rta::net {
struct CacheFileOps {
    FILE* (*open)(const std::filesystem::path&) = [](const std::filesystem::path& p) -> FILE* {
#ifdef _WIN32
        return _wfopen(p.c_str(), L"wb");
#else
        return std::fopen(p.c_str(), "wb");
#endif
    };
    size_t (*write)(const void*, size_t, FILE*) = [](const void* data, size_t size, FILE* f) {
        return std::fwrite(data, 1, size, f);
    };
    int (*close)(FILE*) = [](FILE* f) { return std::fclose(f); };
    void (*publish)(const std::filesystem::path&, const std::filesystem::path&, std::error_code&) =
        [](const auto& from, const auto& to, auto& ec) { std::filesystem::rename(from, to, ec); };
    void (*replace)(const std::filesystem::path&, const std::filesystem::path&, std::error_code&) =
        [](const auto& from, const auto& to, auto& ec) {
#ifdef _WIN32
            if (!rta::platform::atomic_replace(from, to))
                ec = std::error_code(rta::platform::last_error_code(), std::system_category());
#else
            std::filesystem::rename(from, to, ec);
#endif
        };
};

inline bool cache_leaf_valid(const std::string& leaf) {
    if (leaf.empty() || leaf == "." || leaf == ".." || leaf.back() == '.' || leaf.back() == ' ') return false;
    for (unsigned char c : leaf) {
        if (c < 32 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' ||
            c == '\\' || c == '|' || c == '?' || c == '*') return false;
    }
    std::string stem = leaf.substr(0, leaf.find('.'));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL" ||
        stem == "CONIN$" || stem == "CONOUT$") return false;
    if (stem.size() >= 4 && (stem.compare(0, 3, "COM") == 0 || stem.compare(0, 3, "LPT") == 0)) {
        const auto digit = stem.substr(3);
        if ((digit.size() == 1 && digit[0] >= '1' && digit[0] <= '9') ||
            digit == "\xc2\xb9" || digit == "\xc2\xb2" || digit == "\xc2\xb3") return false;
    }
    return true;
}

inline std::string cache_write_new(const std::string& root, const std::string& extension,
                                  const void* bytes, size_t size, std::string& error,
                                  const CacheFileOps& ops = {}, const std::string& leafName = {}) {
    error.clear();
    if (!leafName.empty() && !cache_leaf_valid(leafName)) { error = "非法缓存文件名"; return {}; }
    // Extension is a single ASCII file component; external suffixes must never become paths.
    for (unsigned char c : extension) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) {
            error = "非法缓存扩展名"; return {};
        }
    }
    const auto dir = std::filesystem::u8path(root);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) { error = "无法创建缓存目录: " + ec.message(); return {}; }
    static std::atomic<unsigned long long> sequence{0};
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path object;
    for (;;) {
        object = dir / ("object-" + std::to_string(tick) + "-" + std::to_string(sequence.fetch_add(1)));
        if (std::filesystem::create_directory(object, ec)) break;
        if (ec) { error = "无法分配缓存文件: " + ec.message(); return {}; }
    }
    const auto temp = object / "download.part";
    const auto leaf = leafName.empty() ? ("audio." + (extension.empty() ? "audio" : extension)) : leafName;
    const auto target = object / std::filesystem::u8path(leaf);
    auto cleanup = [&] {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        std::filesystem::remove(object, ignored); // Only our empty directory, never recursive.
    };
    FILE* file = ops.open(temp);
    if (!file) { error = "无法打开缓存文件"; cleanup(); return {}; }
    const bool written = ops.write(bytes, size, file) == size;
    const bool closed = ops.close(file) == 0;
    if (!written || !closed) { error = !written ? "缓存写入不完整" : "缓存关闭失败"; cleanup(); return {}; }
    ops.publish(temp, target, ec);
    if (ec) { error = "缓存发布失败: " + ec.message(); cleanup(); return {}; }
    const auto utf8 = target.u8string();
    return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}
// Checked replacement for a persistent cache. Failure preserves the previous target.
inline bool cache_replace_existing(const std::filesystem::path& target, const std::string& bytes,
                                   std::string& error, const CacheFileOps& ops = {}) {
    const auto parent = target.parent_path().empty() ? std::filesystem::path(".") : target.parent_path();
    const auto root = parent.u8string();
    const auto path = cache_write_new(std::string(reinterpret_cast<const char*>(root.data()), root.size()),
                                      "tmp", bytes.data(), bytes.size(), error, ops);
    if (path.empty()) return false;
    const auto temp = std::filesystem::u8path(path);
    std::error_code ec;
    ops.replace(temp, target, ec);
    if (ec) error = "缓存替换失败: " + ec.message();
    std::error_code ignored;
    if (ec) std::filesystem::remove(temp, ignored);
    std::filesystem::remove(temp.parent_path(), ignored);
    return !ec;
}
} // namespace rta::net
