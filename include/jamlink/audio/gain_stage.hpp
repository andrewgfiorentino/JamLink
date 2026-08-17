// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/audio_block.hpp"
#include "jamlink/audio/realtime_atomic.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace jamlink::audio {

class GainStage final {
public:
    explicit GainStage(float initialLinearGain = 1.0F) noexcept
        : targetGain_(sanitizeGain(initialLinearGain)), currentGain_(sanitizeGain(initialLinearGain)) {}

    // Control-thread API.
    void setLinearGain(float gain) noexcept {
        targetGain_.store(sanitizeGain(gain));
    }

    // Control-thread API.
    void setMuted(bool muted) noexcept {
        muted_.store(muted ? 1U : 0U, std::memory_order_release);
    }

    [[nodiscard]] bool muted() const noexcept {
        return muted_.load(std::memory_order_acquire) != 0U;
    }

    // Real-time API. The gain is ramped once per frame to avoid block-edge clicks.
    void process(InterleavedAudioBlock block) noexcept {
        if (!block.valid()) {
            return;
        }

        const auto frames = block.frameCount();
        if (frames == 0) {
            return;
        }

        const float requested = muted() ? 0.0F : targetGain_.load();
        const float step = (requested - currentGain_) / static_cast<float>(frames);
        float gain = currentGain_;
        std::size_t sampleIndex = 0;

        for (std::size_t frame = 0; frame < frames; ++frame) {
            gain += step;
            for (std::size_t channel = 0; channel < block.channelCount; ++channel) {
                const float processed = block.samples[sampleIndex] * gain;
                block.samples[sampleIndex] = std::isfinite(processed) ? processed : 0.0F;
                ++sampleIndex;
            }
        }

        currentGain_ = requested;
    }

    // Call only while processing is stopped.
    void reset(float currentLinearGain = 0.0F) noexcept {
        currentGain_ = sanitizeGain(currentLinearGain);
    }

private:
    [[nodiscard]] static float sanitizeGain(float gain) noexcept {
        if (!std::isfinite(gain)) {
            return 0.0F;
        }
        return std::clamp(gain, 0.0F, 16.0F);
    }

    RealtimeAtomicFloat targetGain_;
    std::atomic<std::uint32_t> muted_{0};
    float currentGain_{1.0F};
};

} // namespace jamlink::audio
