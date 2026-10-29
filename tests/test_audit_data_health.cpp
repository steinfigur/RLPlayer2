#include "audit_data_fixture.h"
#include "analysis/health_store.h"
#include <iostream>
#include <fstream>
#include <stdexcept>
static int checks = 0;
static void check(bool pass) { ++checks; if (!pass) throw std::runtime_error("health check " + std::to_string(checks)); }
static std::string read(const std::filesystem::path& p) { std::ifstream f(p, std::ios::binary); return {std::istreambuf_iterator<char>(f), {}}; }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string fixture = audit_fixture(argv[1]);
    using namespace rta::analysis;
    std::filesystem::create_directories(std::filesystem::u8path(fixture));
    const std::string cache = fixture + "/health.json";
    TrackHealth original;
    original.drDb = 1.2345;
    original.lufs = 1.2345;
    original.lra = 1.2345;
    original.peakDbfs = 1.2345;
    original.truePeakDbtp = 1.2345;
    original.losslessSuspected = true;
    original.cutoffHz = 1.2345;
    original.confidence = 1.2345;
    original.fakeVerdict = 2;
    original.edgeDb = 1.2345;
    original.windows = 2;
    original.windowFill = 1.2345;
    original.reason = "中文😀\"\\test";
    original.fake24bit = true;
    original.durationSec = 1.2345;
    original.fs = 2;
    original.channels = 2;
    original.bits = 2;
    original.status = "中文😀\"\\test";
    original.error = "中文😀\"\\test";
    original.declaredSec = 1.2345;
    original.incomplete = true;
    original.note = "中文😀\"\\test";
    original.analyzedAtMs = 2;
    original.hasQuality = true;
    original.quality.thdnDb = 1.2345;
    original.quality.thdnPct = 1.2345;
    original.quality.thdnAvailable = true;
    original.quality.thdnMethod = "中文😀\"\\test";
    original.quality.nonTonalDb = 1.2345;
    original.quality.imdAvailable = true;
    original.quality.imdDb = 1.2345;
    original.quality.imdNote = "中文😀\"\\test";
    original.quality.noiseFloorDbfs = 1.2345;
    original.quality.spectralMinDbfs = 1.2345;
    original.quality.snrDb = 1.2345;
    original.quality.centroidHz = 1.2345;
    original.quality.tiltDbPerOct = 1.2345;
    original.quality.rolloff95Hz = 1.2345;
    original.quality.hfRatio10kDb = 1.2345;
    original.quality.peakDbfs = 1.2345;
    original.quality.rmsDbfs = 1.2345;
    original.quality.clippedSamples = 2;
    original.quality.clippedPct = 1.2345;
    original.quality.clippedRuns = 2;
    original.quality.longestClipRun = 2;
    original.quality.crestDb = 1.2345;
    original.quality.stereo = true;
    original.quality.channelCorr = 1.2345;
    original.quality.monoLossDb = 1.2345;
    original.quality.sideMidDb = 1.2345;
    original.quality.dynP95P10Db = 1.2345;
    original.quality.dynCentroidDbfs = 1.2345;
    original.quality.quietP10Dbfs = 1.2345;
    original.quality.silencePct = 1.2345;
    original.quality.spectralWindows = 2;
    original.quality.note = "中文😀\"\\test";
    original.status = "incomplete";
    HealthStore store(cache); check(store.loadError().empty());
    store.upsert("fixture.wav", 111, 222, original); check(store.save());
    HealthStore loaded(cache); TrackHealth restored;
    check(loaded.lookup("fixture.wav", 111, 222, restored));
    check(track_health_json(restored) == track_health_json(original));
    check(restored.hasQuality && restored.quality.valid && restored.incomplete && restored.fakeVerdict == 2);
    check(!loaded.lookup("fixture.wav", 112, 222, restored));
    TrackHealth noQuality = original; noQuality.hasQuality = false;
    store.upsert("fixture.wav", 111, 222, noQuality); check(store.save());
    HealthStore unavailable(cache);
    check(unavailable.lookup("fixture.wav", 111, 222, restored) && !restored.hasQuality && !restored.quality.valid &&
          track_health_json(restored) == track_health_json(noQuality));
    std::string saved = read(std::filesystem::u8path(cache));
    const auto schema = saved.find("\"schema\":2,"); check(schema != std::string::npos);
    auto oversized = saved;
    const auto mtimeBegin = oversized.find("\"mtime\":") + 8;
    const auto mtimeEnd = oversized.find(',', mtimeBegin);
    oversized.replace(mtimeBegin, mtimeEnd - mtimeBegin, "9223372036854775808");
    { std::ofstream f(std::filesystem::u8path(cache)); f << oversized; }
    HealthStore unsafeInteger(cache); check(!unsafeInteger.lookup("fixture.wav", 111, 222, restored) && unsafeInteger.skippedEntries() == 1);
    auto legacy = saved; legacy.erase(schema, 11);
    { std::ofstream f(std::filesystem::u8path(cache)); f << legacy; }
    HealthStore old(cache); check(!old.lookup("fixture.wav", 111, 222, restored) && old.skippedEntries() == 1);
    { std::ofstream f(std::filesystem::u8path(cache)); f << "[{\"path\":\"partial"; }
    const auto damaged = read(std::filesystem::u8path(cache)); HealthStore broken(cache);
    check(!broken.loadError().empty() && !broken.save() && read(std::filesystem::u8path(cache)) == damaged);
    std::cout << "PASS health " << checks << " checks serialized fields=" << 57 << "\n";
}
