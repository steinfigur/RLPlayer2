// One integrity conclusion for signal-chain/status consumers; no config guesses or I/O.
#pragma once
#include "pipeline.h"
#include <string>
#include <vector>
namespace rta::engine {
struct IntegritySource {
    int actualBits = 0;       // Original integer precision, not the decoder container; 0 means unknown.
    int containerBits = 0;
    int channels = 0;
    bool isFloat = false;
};
struct IntegrityOutput {
    std::string mode = "pcm"; // pcm / dop / native
    bool directDsd = false;    // Actual unmodified one-bit branch, never the requested setting.
    double liveGainDb = 0.0;   // Gain actually applied after the Pipeline.
};
struct PlaybackIntegrity {
    std::string state;         // unknown / yes / no / direct (existing UI vocabulary)
    std::string reason;
};
inline PlaybackIntegrity playback_integrity(const PipelineStats& st,
                                             const IntegritySource& source,
                                             const IntegrityOutput& output = {}) {
    if (st.outFrames == 0) return {"unknown", "等待实际输出样本"};
    std::vector<std::string> why;
    if (st.usedMatrix) why.emplace_back("Matrix 路由");
    if (st.usedEq) why.emplace_back("EQ");
    if (st.usedPeq) why.emplace_back("PEQ");
    if (st.usedDelay) why.emplace_back("声道延迟");
    if (st.usedConvolution || st.usedGpuConvolution) why.emplace_back("IR 卷积");
    if (st.usedResample) why.emplace_back("重采样");
    if (st.usedVolume) why.emplace_back("音量");
    if (st.usedChannelGain) why.emplace_back("每声道增益");
    if (st.usedPreAtten) why.emplace_back("预衰减");
    if (st.usedReplayGain) why.emplace_back("ReplayGain");
    if (st.usedLoudness) why.emplace_back("等响度补偿");
    if (st.usedShaper) why.emplace_back("抖动/整形");
    if (!rta::audio::audio_finite(output.liveGainDb)) why.emplace_back("输出增益非法");
    else if (output.liveGainDb != 0.0) why.emplace_back("实时音量");
    if (source.channels > 0 && st.outChannels > 0 && source.channels != st.outChannels)
        why.emplace_back("声道数改变");
    const bool dsdOutput = output.mode == "dop" || output.mode == "native";
    if (output.directDsd) {
        if (!dsdOutput || source.actualBits != 1 || source.isFloat)
            why.emplace_back("DSD 直通源/输出模式不一致");
    } else if (dsdOutput) {
        why.emplace_back("DSD 重调制");
    } else if (output.mode == "pcm") {
        if (source.actualBits == 1) why.emplace_back("DSD 源解码/抽取为 PCM");
        if (source.isFloat && !st.bitPerfect) why.emplace_back("浮点源转整数输出");
        if (st.inRate && st.outRate && st.inRate != st.outRate && !st.usedResample)
            why.emplace_back("采样率改变");
        if (source.actualBits > 0 && st.outBits > 0 && st.outBits < source.actualBits)
            why.emplace_back("真实位深降低");
    }
    if (!why.empty()) {
        std::string reason;
        for (const auto& item : why) { if (!reason.empty()) reason += "、"; reason += item; }
        return {"no", reason};
    }
    if (output.mode != "pcm" && !dsdOutput) return {"unknown", "输出模式无法核验"};
    if (source.actualBits <= 0 || source.containerBits < source.actualBits ||
        source.channels <= 0 || st.outChannels <= 0)
        return {"unknown", "源真实位深或声道信息不足"};
    if (output.directDsd) return {"direct", "DSD 有效位直通，未经过 PCM 或重新调制"};
    if (!st.inRate || !st.outRate || st.outBits <= 0 || st.inBits != source.containerBits)
        return {"unknown", "实际输出或解码容器信息不足"};
    if (source.isFloat) return {"yes", "浮点 PCM 逐位直通（实际 bitPerfect 路径）"};
    if (st.outBits > source.actualBits)
        return {"yes", "整数 PCM 无损展宽（按源真实位深核验）"};
    return {"yes", "整数 PCM 有效位保持（按源真实位深核验）"};
}
} // namespace rta::engine
