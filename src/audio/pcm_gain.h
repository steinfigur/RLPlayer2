// PCM live gain, shared by the ASIO callback and offline contract tests.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>
namespace rta::audio {
inline void apply_pcm_live_gain(int32_t* samples, size_t count, int bits, double gain) {
    if (gain == 1.0) return;
    const double full = std::ldexp(1.0, bits - 1);
    for (size_t i = 0; i < count; ++i) {
        const double v = std::round((double)samples[i] * gain);
        samples[i] = (int32_t)std::clamp(v, -full, full - 1.0);
    }
}
}
