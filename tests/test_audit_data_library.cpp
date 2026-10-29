#include "audit_data_fixture.h"
// Actual Library worker/publication path; only the audio metadata probe is deterministic.
#include <string>
#include <filesystem>
std::filesystem::file_status audit_status(const std::string&, std::error_code&);
#define RTA_LIBRARY_TEST_STATUS(path, error) audit_status(path, error)
namespace rta::net { struct LibTrack; }
rta::net::LibTrack audit_probe(const std::string&);
#define RTA_LIBRARY_TEST_PROBE(file) audit_probe(file)
#include "net/cache_file.h"
static rta::net::CacheFileOps auditOps;
#define RTA_LIBRARY_TEST_FILE_OPS auditOps
#include "net/library.h"
#include <iostream>
#include <stdexcept>
static std::atomic<int> mode{0}, active{0}, maxActive{0};
static std::atomic<bool> statusError{false};
std::filesystem::file_status audit_status(const std::string& path, std::error_code& error) {
    const auto p = std::filesystem::u8path(path);
    if (statusError.load() && p.filename() == "1.wav") { error = std::make_error_code(std::errc::permission_denied); return {}; }
    return std::filesystem::status(p, error);
}
static int checks = 0;
static void check(bool pass) { ++checks; if (!pass) throw std::runtime_error("library check " + std::to_string(checks)); }
rta::net::LibTrack audit_probe(const std::string& file) {
    const int count = active.fetch_add(1) + 1; int previous = maxActive.load();
    while (previous < count && !maxActive.compare_exchange_weak(previous, count)) {}
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    active.fetch_sub(1);
    if (mode.load() == 1 && std::filesystem::u8path(file).filename() == "new.wav") throw std::runtime_error("AUDIT_WORKER_FAILURE");
    rta::net::LibTrack result; result.path = file; result.title = mode.load() != 0 ? "PARTIAL_MUST_NOT_PUBLISH" : file; result.album = "AUDIT";
    result.sampleRate = 48000; result.bits = 24; result.channels = 2; result.duration = 1;
    return result;
}
static std::string read(const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string fixture = audit_fixture(argv[1]);
    using namespace rta::net;
    const auto base = std::filesystem::u8path(fixture); const auto music = base / "music";
    std::filesystem::create_directories(music);
    _putenv_s("RLPLAYER_LIBRARY_DIR", fixture.c_str()); _putenv_s("RLPLAYER_PORT", "8899");
    for (int i = 0; i != 48; ++i) { std::ofstream f(music / (std::to_string(i) + ".wav")); f << "fixture"; }
    auto& library = Library::instance(); check(library.addRoot(rta::meta::meta_string_of(music)));
    library.setScanThreads(4);
    const auto run = [&] {
        library.startScan(true); int previous = 0;
        while (library.scanning()) { const int count = library.scanned(); check(count >= previous); previous = count; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
        library.joinScan();
    };
    run(); check(library.trackCount() == 48 && maxActive.load() > 1 && library.scanMsg().find("完成") != std::string::npos);
    const auto index = base / "rlplayer-index.tsv"; const std::string before = read(index); check(!before.empty());
    { std::ofstream f(music / "new.wav"); f << "new fixture"; }
    mode.store(1); run();
    check(library.trackCount() == 48 && read(index) == before);
    check(library.scanMsg().find("AUDIT_WORKER_FAILURE") != std::string::npos && library.scanMsg().find("完成") == std::string::npos);
    std::filesystem::remove(music / "0.wav"); statusError.store(true); run();
    check(library.trackCount() == 47 && library.lastScanRemoved() == 1);
    const auto pruned = read(index);
    check(pruned.find("PARTIAL_MUST_NOT_PUBLISH") == std::string::npos && pruned.find(rta::meta::meta_string_of(music / "0.wav") + "\t") == std::string::npos);
    check(lib_file_exists(rta::meta::meta_string_of(music / "1.wav"))); // Permission error retains old healthy record.
    check(!lib_file_exists(rta::meta::meta_string_of(music / "missing.wav")));
    statusError.store(false);
    mode.store(0); run(); check(library.trackCount() == 48 && library.scanMsg().find("完成") != std::string::npos);
    const auto healthy = read(index);
    library.startScan(true); std::this_thread::sleep_for(std::chrono::milliseconds(45)); library.stopScan(); library.joinScan();
    check(library.trackCount() == 48 && read(index) == healthy);
    const auto persisted = read(index);
    LibTrack oldTrack; check(library.trackAt(0, oldTrack));
    mode.store(2);
    for (int fault = 0; fault != 5; ++fault) {
        auditOps = CacheFileOps{};
        if (fault == 0) auditOps.open = [](const auto&) -> FILE* { return nullptr; };
        if (fault == 1) auditOps.write = [](const void* p, size_t n, FILE* f) { return std::fwrite(p, 1, n / 2, f); };
        if (fault == 2) auditOps.close = [](FILE* f) { std::fclose(f); return EOF; };
        if (fault == 3) auditOps.publish = [](const auto&, const auto&, auto& ec) { ec = std::make_error_code(std::errc::permission_denied); };
        if (fault == 4) auditOps.replace = [](const auto&, const auto&, auto& ec) { ec = std::make_error_code(std::errc::permission_denied); };
        run(); LibTrack current; check(library.trackAt(0, current));
        check(read(index) == persisted && library.trackCount() == 48 && current.title == oldTrack.title);
        check(library.scanMsg().find("写入曲库失败") != std::string::npos && library.scanMsg().find("完成") == std::string::npos);
    }
    auditOps = CacheFileOps{}; mode.store(0);
    // A concurrent explicit removal must win over the scan's stale starting snapshot.
    library.startScan(true);
    while (active.load() == 0 && library.scanning()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    check(library.removePath(rta::meta::meta_string_of(music / "1.wav")));
    const auto removedState = read(index); library.joinScan();
    check(library.trackCount() == 47 && read(index) == removedState && library.scanMsg().find("索引已变更") != std::string::npos);
    // Prune owns an independent complete publication as well.
    library.startScan(true);
    while (active.load() == 0 && library.scanning()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    std::filesystem::remove(music / "2.wav");
    check(library.pruneMissing().removed == 1);
    const auto prunedState = read(index); library.joinScan();
    check(library.trackCount() == 46 && read(index) == prunedState && library.scanMsg().find("索引已变更") != std::string::npos);
    std::cout << "PASS library " << checks << " checks max_parallel=" << maxActive.load() << "\n";
}
