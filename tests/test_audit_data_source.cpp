#include "audit_data_fixture.h"
// Actual SourceManager, with only transport/platform-directory adapters replaced.
#include "net/cache_file.h"
static rta::net::CacheFileOps auditOps;
#define RTA_SOURCE_TEST_FILE_OPS auditOps
#include "net/source_mgr.h"
#include <thread>
#include <iostream>
#include <stdexcept>
static int checks = 0;
static void check(bool pass) { ++checks; if (!pass) throw std::runtime_error("source check " + std::to_string(checks)); }
static std::string read(const std::string& p) { std::ifstream f(std::filesystem::u8path(p), std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string fixture = audit_fixture(argv[1]);
#ifdef _WIN32
    _putenv_s("RTA_AUDIT_SOURCE_ROOT", fixture.c_str());
#else
    setenv("RTA_AUDIT_SOURCE_ROOT", fixture.c_str(), 1);
#endif
    std::filesystem::create_directories(std::filesystem::u8path(fixture));
    using namespace rta::net;
    auto& m = SourceManager::instance(); m.load(fixture + "/fake-only.conf");
    SourceConfig a; a.base = "serverA"; a.user = "fakeUser"; a.pass = "FAKE_password";
    const auto aid = m.add_server(a); auto b = a; b.base = "serverB"; b.kind = "emby"; const auto bid = m.add_server(b);
    std::string error;
    auto first = m.fetch("42", "flac", error); check(read(first) == "serverA:fakeUser:FAKE_password:42");
    check(m.fetch("42", "flac", error) == first && downloads == 1);
    m.set_active(bid); auto second = m.fetch("42", "flac", error); check(read(second) == "serverB:fakeUser:FAKE_password:42");
    for (const std::string id : {"a:b", "a?b", "中文", "A", "a", "a/b"}) check(read(m.fetch(id, "flac", error)) == "serverB:fakeUser:FAKE_password:" + id);
    m.set_active(aid); a.user = "anotherUser"; a.pass = "FAKE_second"; check(m.update_server(aid, a, false));
    auto changed = m.fetch("42", "flac", error); check(read(changed) == "serverA:anotherUser:FAKE_second:42" && changed != first);
    std::thread switcher([&] { for (int i = 0; i != 100; ++i) m.set_active((i & 1) ? aid : bid); });
    std::thread fetcher([&] { for (int i = 0; i != 100; ++i) { std::string e; m.fetch("concurrent", "flac", e); } });
    switcher.join(); fetcher.join();
    m.set_active(aid); check(read(m.fetch("concurrent", "flac", error)) == "serverA:anotherUser:FAKE_second:concurrent");
    m.set_active(bid); check(read(m.fetch("concurrent", "flac", error)) == "serverB:fakeUser:FAKE_password:concurrent");
    check(read(first) == "serverA:fakeUser:FAKE_password:42");
    const auto corrupt = m.fetch("corrupt", "flac", error); { std::ofstream f(std::filesystem::u8path(corrupt)); f << "x"; }
    check(read(m.fetch("corrupt", "flac", error)) == "serverB:fakeUser:FAKE_password:corrupt");
    const size_t before = m.cache_count();
    for (int fault = 0; fault != 4; ++fault) {
        auditOps = CacheFileOps{};
        if (fault == 0) auditOps.open = [](const auto&) -> FILE* { return nullptr; };
        if (fault == 1) auditOps.write = [](const void* p, size_t n, FILE* f) { return std::fwrite(p, 1, n / 2, f); };
        if (fault == 2) auditOps.close = [](FILE* f) { std::fclose(f); return EOF; };
        if (fault == 3) auditOps.publish = [](const auto&, const auto&, auto& ec) { ec = std::make_error_code(std::errc::permission_denied); };
        check(m.fetch("fault-" + std::to_string(fault), "flac", error).empty() && !error.empty() && m.cache_count() == before);
        check(read(first) == "serverA:fakeUser:FAKE_password:42");
    }
    auditOps = CacheFileOps{};
    m.set_active(aid);
    auditOps.publish = [](const auto& from, const auto& to, auto& ec) {
        // Switch active server deterministically after download, before registration.
        auto& manager = SourceManager::instance();
        for (const auto& server : manager.servers()) if (server.base == "serverB") manager.set_active(server.id);
        std::filesystem::rename(from, to, ec);
    };
    const auto snapshot = m.fetch("snapshot", "flac", error);
    auditOps = CacheFileOps{};
    check(read(snapshot) == "serverA:anotherUser:FAKE_second:snapshot");
    check(read(m.fetch("snapshot", "flac", error)) == "serverB:fakeUser:FAKE_password:snapshot");
    const size_t afterSnapshot = m.cache_count();
    check(m.fetch("escape", "../flac", error).empty() && !error.empty() && m.cache_count() == afterSnapshot);
    const auto dir = std::filesystem::u8path(fixture) / "remote-cache";
    const auto held = std::filesystem::u8path(fixture) / "held-cache";
    std::filesystem::rename(dir, held); { std::ofstream f(dir); f << "block directory"; }
    check(m.fetch("write-failure", "flac", error).empty() && !error.empty() && m.cache_count() == afterSnapshot);
    check(read(second) == ""); // It was moved intact, rather than overwritten by the failed new write.
    std::filesystem::remove(dir); std::filesystem::rename(held, dir);
    check(read(second) == "serverB:fakeUser:FAKE_password:42");
    std::cout << "PASS source " << checks << " checks\n";
}
