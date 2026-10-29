#include "audit_data_fixture.h"
#include "net/cache_file.h"
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
using namespace rta::net;
static std::string read(const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
static int checks = 0;
static void check(bool pass) { ++checks; if (!pass) throw std::runtime_error("writer check " + std::to_string(checks)); }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string fixture = audit_fixture(argv[1]);
    const auto root = std::filesystem::u8path(fixture);
    std::filesystem::create_directories(root);
    const auto target = root / "persistent.json";
    const std::string rootString = fixture, data(16384, 'N');
    std::string error;
    check(cache_replace_existing(target, "OLD", error));
    for (int fault = 0; fault != 5; ++fault) {
        CacheFileOps ops;
        if (fault == 0) ops.open = [](const auto&) -> FILE* { return nullptr; };
        if (fault == 1) ops.write = [](const void* p, size_t n, FILE* f) { return std::fwrite(p, 1, n / 2, f); };
        if (fault == 2) ops.close = [](FILE* f) { std::fclose(f); return EOF; };
        if (fault == 3) ops.publish = [](const auto&, const auto&, auto& ec) { ec = std::make_error_code(std::errc::permission_denied); };
        if (fault == 4) ops.replace = [](const auto&, const auto&, auto& ec) { ec = std::make_error_code(std::errc::permission_denied); };
        check(!cache_replace_existing(target, data, error, ops));
        check(!error.empty() && read(target) == "OLD");
        size_t count = 0; for (const auto& e : std::filesystem::directory_iterator(root)) { (void)e; ++count; }
        check(count == 1); // Only the pre-existing persistent target; all our temporary paths removed.
    }
    check(cache_replace_existing(target, data, error) && read(target) == data);
    const std::string named = cache_write_new(rootString, "flac", data.data(), data.size(), error, {}, "中文😀.flac");
    check(!named.empty() && read(std::filesystem::u8path(named)) == data);
    const auto filename = std::filesystem::u8path(named).filename().u8string();
    check(std::string(reinterpret_cast<const char*>(filename.data()), filename.size()) == "中文😀.flac");
    for (const auto& invalid : {".", "..", "../x.flac", "x\\a", "C:a", "x?y", "x.", "x ", "CON", "con.flac", "NUL.wav", "COM1.flac", "LPT9.flac", "CONOUT$.flac"})
        check(cache_write_new(rootString, "flac", data.data(), data.size(), error, {}, invalid).empty() && !error.empty());
    check(cache_write_new(rootString, "../flac", data.data(), data.size(), error).empty());
    std::vector<std::string> paths(16); std::vector<std::thread> workers;
    for (int i = 0; i != 16; ++i) workers.emplace_back([&, i] {
        std::string localError; const std::string bytes(16384, char('A' + i));
        paths[i] = cache_write_new(rootString, "flac", bytes.data(), bytes.size(), localError);
    });
    for (auto& worker : workers) worker.join();
    for (int i = 0; i != 16; ++i) check(!paths[i].empty() && read(std::filesystem::u8path(paths[i])) == std::string(16384, char('A' + i)));
    std::cout << "PASS writer " << checks << " checks\n";
}
