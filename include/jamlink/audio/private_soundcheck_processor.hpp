// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/realtime_atomic.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jamlink::audio {

enum class SoundcheckSource : std::uint8_t {
    Instrument,
    Voice
};

struct PrivateSoundcheckInputs final {
    std::span<const float> instrumentMono;
    std::span<const float> voiceMono;
};

// This processor has no transport dependency or network-send API. Full graph
// isolation is enforced separately by AudioGraphPurpose::PrivateSoundcheck.
class PrivateSoundcheckProcessor final {
public:
    PrivateSoundcheckProcessor() noexcept;

    // Control-thread API.
    void setMonitorEnabled(SoundcheckSource source, bool enabled) noexcept;
    void setMonitorGain(SoundcheckSource source, float linearGain) noexcept;

    // Real-time API. Always overwrites the caller-provided stereo monitor span.
    void process(
        const PrivateSoundcheckInputs& inputs,
        std::span<float> stereoMonitorOutput) noexcept;

    // Call only while processing is stopped.
    void reset() noexcept;

private:
    struct SourceState final {
        RealtimeAtomicFloat targetGain{1.0F};
        std::atomic<std::uint32_t> enabled{1U};
        float currentGain{0.0F};
    };

    [[nodiscard]] static float sanitizeGain(float gain) noexcept;
    [[nodiscard]] static float nextGain(SourceState& state) noexcept;

    SourceState instrument_;
    SourceState voice_;
};

} // namespace jamlink::audio
