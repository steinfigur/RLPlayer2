// health_store.h — 音频体检 B1b：按 (规范路径, mtime, size) 三元组为键的健康缓存
//
// 设计要点：
//  1) 键 = std::filesystem::weakly_canonical 规范化后的路径（generic 形式，'/' 分隔），
//     命中还须 mtime、size 逐字相等 —— 文件被改写/替换后自然失效，不需要 TTL。
//     mtime 取「自时钟纪元的毫秒数」而不是原生 count：JSON number 走 double（53 位尾数），
//     MSVC file_clock 的 100ns count 约 1.3e17，超出 2^53 会失真导致缓存永不命中（踩坑点）。
//  2) 序列化复用 track_health_json（单条体检的对象体），外层是数组，每条
//     {"path":..,"mtime":..,"size":..,"health":{..}}；解析用 src/net/json.h（只读解析器，
//     项目约定不引第三方 JSON 库）。
//  3) 损坏文件如实报错、绝不静默清空：构造时解析失败 → loadError() 非空，此时
//     save() 拒绝写盘（防止用一个空库把用户缓存覆盖掉），需要人工删掉/修复缓存文件。
//  4) save() 原子写：独立临时对象 + 平台原子替换，检查 write/close；失败不删除旧缓存。
//  5) schema=2 覆盖完整 track_health_json 字段；旧条目缓存未命中，重新分析后升级。
#pragma once
#include "track_health.h"
#include "../audio/audio_loader.h"   // rta::audio::widen（UTF-8 → UTF-16，中文缓存路径必需）
#include "../net/cache_file.h"
#include "../net/json.h"             // rta::net::json_parse（只读解析）
#include <filesystem>
#include <map>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <limits>
#include <type_traits>

namespace rta::analysis {

// 路径规范化：weakly_canonical 失败（如悬空盘符）时退回原串，绝不抛异常。
inline std::string normalize_path_key(const std::string& path) {
    try {
        return std::filesystem::weakly_canonical(rta::audio::widen(path)).generic_string();
    } catch (...) {
        return path;
    }
}

class HealthStore {
public:
    // jsonPath 不存在 → 空库（不是错误）；存在但损坏/为空 → loadError() 非空且拒绝 save()。
    explicit HealthStore(const std::string& jsonPath) : jsonPath_(jsonPath) {
        if (jsonPath_.empty()) { loadError_ = "缓存路径为空"; return; }
        std::string text, readErr;
        if (!read_whole_file(jsonPath_, &text, &readErr)) {
            loadError_ = readErr;      // 文件不存在时 readErr 为空 → 空库，不算错误
            return;
        }
        const rta::net::Json root = rta::net::json_parse(text);
        if (root.type != rta::net::Json::Arr) {
            loadError_ = "缓存文件损坏（不是合法 JSON 或根不是数组），已按空库处理且不会回写: " + jsonPath_;
            return;
        }
        for (size_t i = 0; i < root.size(); ++i) {
            const rta::net::Json& e = root.at(i);
            if (e.type != rta::net::Json::Obj) { ++skipped_; continue; }
            const std::string p = e["path"].s();
            const rta::net::Json& hj = e["health"];
            if (p.empty() || hj.type != rta::net::Json::Obj || hj["status"].s().empty()) { ++skipped_; continue; }
            Entry en;
            if (!read_number(e["mtime"], en.mtime) || !read_number(e["size"], en.size)) { ++skipped_; continue; }
            // Schema 2 is the full serialized analysis contract. Legacy entries are cache misses,
            // never fabricated measurements (e.g. missing true peak must not become measured 0).
            if (e["schema"].type != rta::net::Json::Num || e["schema"].n() != 2 ||
                !decode_health(hj, en.health)) { ++skipped_; continue; }
            entries_[normalize_path_key(p)] = std::move(en);
        }
    }

    // 命中 = 路径（规范化后）+ mtime + size 三者全等。返回 false 表示需重算。
    bool lookup(const std::string& path, int64_t mtime, int64_t size, TrackHealth& out) const {
        if (!loadError_.empty()) return false;
        const auto it = entries_.find(normalize_path_key(path));
        if (it == entries_.end()) return false;
        if (it->second.mtime != mtime || it->second.size != size) return false;
        out = it->second.health;
        return true;
    }

    void upsert(const std::string& path, int64_t mtime, int64_t size, const TrackHealth& h) {
        Entry en;
        en.mtime = mtime;
        en.size = size;
        en.health = h;
        entries_[normalize_path_key(path)] = std::move(en);
    }

    // 原子写（tmp + rename）。加载时报过错的库拒绝写盘（见类头注释），返回 false。
    bool save() {
        if (!loadError_.empty()) return false;
        if (jsonPath_.empty()) return false;
        std::string data = "[";
        bool first = true;
        for (const auto& kv : entries_) {
            if (!first) data += ",";
            first = false;
            data += "{\"schema\":2,\"path\":\"" + detail::json_escape(kv.first) + "\"";
            data += ",\"mtime\":" + std::to_string((long long)kv.second.mtime);
            data += ",\"size\":" + std::to_string((long long)kv.second.size);
            data += ",\"health\":" + track_health_json(kv.second.health);
            data += "}";
        }
        data += "]";
        std::string error;
        return rta::net::cache_replace_existing(std::filesystem::u8path(jsonPath_), data, error);
    }

    size_t size() const { return entries_.size(); }
    const std::string& loadError() const { return loadError_; }
    int skippedEntries() const { return skipped_; }   // 解析时跳过的损坏条数（非致命）

private:
    template<class T> static bool read_number(const rta::net::Json& j, T& out) {
        if (j.type != rta::net::Json::Num) return false;
        const long double value = j.num;
        if (value < (long double)std::numeric_limits<T>::lowest() ||
            value > (long double)std::numeric_limits<T>::max()) return false;
        if constexpr (std::is_integral<T>::value) {
            if (std::floor(value) != value || value < -9007199254740991.0L || value > 9007199254740991.0L) return false;
        }
        out = static_cast<T>(j.num); return true;
    }
    static bool read_bool(const rta::net::Json& j, bool& out) {
        if (j.type != rta::net::Json::Bool) return false;
        out = j.b; return true;
    }
    static bool read_string(const rta::net::Json& j, std::string& out) {
        if (j.type != rta::net::Json::Str) return false;
        out = j.str; return true;
    }
    static bool decode_health(const rta::net::Json& j, TrackHealth& h) {
        const auto& q = j["quality"];
        if (q.type != rta::net::Json::Obj) return false;
        if (!read_number(j["dr_db"], h.drDb)) return false;
        if (!read_number(j["lufs"], h.lufs)) return false;
        if (!read_number(j["lra"], h.lra)) return false;
        if (!read_number(j["peak_dbfs"], h.peakDbfs)) return false;
        if (!read_number(j["true_peak_dbtp"], h.truePeakDbtp)) return false;
        if (!read_number(j["cutoff_hz"], h.cutoffHz)) return false;
        if (!read_number(j["confidence"], h.confidence)) return false;
        if (!read_number(j["edge_db"], h.edgeDb)) return false;
        if (!read_number(j["window_fill"], h.windowFill)) return false;
        if (!read_number(j["duration_sec"], h.durationSec)) return false;
        if (!read_number(j["declared_sec"], h.declaredSec)) return false;
        if (!read_number(j["spectral_windows"], h.windows)) return false;
        if (!read_number(j["fs"], h.fs)) return false;
        if (!read_number(j["channels"], h.channels)) return false;
        if (!read_number(j["bits"], h.bits)) return false;
        if (!read_number(j["analyzed_at_ms"], h.analyzedAtMs)) return false;
        if (!read_bool(j["lossless_suspected"], h.losslessSuspected)) return false;
        if (!read_bool(j["fake24"], h.fake24bit)) return false;
        if (!read_bool(j["incomplete"], h.incomplete)) return false;
        if (!read_string(j["reason"], h.reason)) return false;
        if (!read_string(j["status"], h.status)) return false;
        if (!read_string(j["error"], h.error)) return false;
        if (!read_string(j["note"], h.note)) return false;
        if (!read_number(q["thdn_db"], h.quality.thdnDb)) return false;
        if (!read_number(q["thdn_pct"], h.quality.thdnPct)) return false;
        if (!read_number(q["non_tonal_db"], h.quality.nonTonalDb)) return false;
        if (!read_number(q["imd_db"], h.quality.imdDb)) return false;
        if (!read_number(q["noise_floor_dbfs"], h.quality.noiseFloorDbfs)) return false;
        if (!read_number(q["spectral_min_dbfs"], h.quality.spectralMinDbfs)) return false;
        if (!read_number(q["snr_db"], h.quality.snrDb)) return false;
        if (!read_number(q["centroid_hz"], h.quality.centroidHz)) return false;
        if (!read_number(q["tilt_db_per_oct"], h.quality.tiltDbPerOct)) return false;
        if (!read_number(q["rolloff95_hz"], h.quality.rolloff95Hz)) return false;
        if (!read_number(q["hf_ratio_10k_db"], h.quality.hfRatio10kDb)) return false;
        if (!read_number(q["peak_dbfs_q"], h.quality.peakDbfs)) return false;
        if (!read_number(q["rms_dbfs"], h.quality.rmsDbfs)) return false;
        if (!read_number(q["clipped_pct"], h.quality.clippedPct)) return false;
        if (!read_number(q["crest_db"], h.quality.crestDb)) return false;
        if (!read_number(q["channel_corr"], h.quality.channelCorr)) return false;
        if (!read_number(q["mono_loss_db"], h.quality.monoLossDb)) return false;
        if (!read_number(q["side_mid_db"], h.quality.sideMidDb)) return false;
        if (!read_number(q["dyn_p95_p10_db"], h.quality.dynP95P10Db)) return false;
        if (!read_number(q["dyn_centroid_dbfs"], h.quality.dynCentroidDbfs)) return false;
        if (!read_number(q["quiet_p10_dbfs"], h.quality.quietP10Dbfs)) return false;
        if (!read_number(q["silence_pct"], h.quality.silencePct)) return false;
        if (!read_number(q["clipped_samples"], h.quality.clippedSamples)) return false;
        if (!read_number(q["clipped_runs"], h.quality.clippedRuns)) return false;
        if (!read_number(q["longest_clip_run"], h.quality.longestClipRun)) return false;
        if (!read_number(q["spectral_windows"], h.quality.spectralWindows)) return false;
        if (!read_bool(q["thdn_available"], h.quality.thdnAvailable)) return false;
        if (!read_bool(q["imd_available"], h.quality.imdAvailable)) return false;
        if (!read_bool(q["stereo"], h.quality.stereo)) return false;
        if (!read_string(q["thdn_method"], h.quality.thdnMethod)) return false;
        if (!read_string(q["imd_note"], h.quality.imdNote)) return false;
        if (!read_string(q["note"], h.quality.note)) return false;
        if (!read_bool(q["available"], h.hasQuality)) return false;
        h.quality.valid = h.hasQuality;
        const std::string verdict = j["fake_verdict"].s();
        if (verdict == "none") h.fakeVerdict = 0;
        else if (verdict == "lossy") h.fakeVerdict = 1;
        else if (verdict == "band_limited") h.fakeVerdict = 2;
        else if (verdict == "unclear") h.fakeVerdict = 3;
        else return false;
        return true;
    }

    struct Entry {
        int64_t mtime = 0;
        int64_t size = 0;
        TrackHealth health;
    };

    // 读整个文件。文件不存在返回 false 且 *err 为空（= 合法的空库）；其他读失败 *err 带原因。
    static bool read_whole_file(const std::string& path, std::string* out, std::string* err) {
        out->clear();
        err->clear();
        FILE* f = _wfopen(rta::audio::widen(path).c_str(), L"rb");   // 宽字符打开：中文路径不落 ANSI 代码页
        if (!f) {
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (GetFileAttributesExW(rta::audio::widen(path).c_str(), GetFileExInfoStandard, &fa))
                *err = "无法读取缓存文件（可能无权限或被占用）: " + path;
            else {
                const DWORD code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                    *err = "无法查询缓存文件: " + path + " (" + std::to_string(code) + ")";
            }
            return false;
        }
        std::string data;
        char buf[65536];
        size_t got = 0;
        while ((got = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, got);
        const bool ioErr = std::ferror(f) != 0;
        std::fclose(f);
        if (ioErr) { *err = "读取缓存文件失败（I/O 错误）: " + path; return false; }
        // 空文件也按「损坏」处理：正常 save() 永远写完整 JSON，不该出现 0 字节
        if (data.empty()) { *err = "缓存文件为空（0 字节，正常不应出现）: " + path; return false; }
        *out = std::move(data);
        return true;
    }

    std::string jsonPath_;
    std::map<std::string, Entry> entries_;   // 键 = normalize_path_key
    std::string loadError_;                  // 非空 = 加载失败（此时 save() 拒绝写盘）
    int skipped_ = 0;
};

} // namespace rta::analysis
