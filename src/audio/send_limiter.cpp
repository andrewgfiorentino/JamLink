// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/send_limiter.hpp"

#include <algorithm>
#include <cmath>

namespace jamlink::audio {

SendLimiter::SendLimiter(
    std::uint32_t sampleRate, float threshold, float releaseSeconds) noexcept {
    prepare(sampleRate, threshold, releaseSeconds);
}

void SendLimiter::prepare(
    std::uint32_t sampleRate, float threshold, float releaseSeconds) noexcept {
    threshold_ = std::clamp(threshold, 0.05F, 1.0F);
    const float rate = sampleRate == 0U ? 48'000.0F : static_cast<float>(sampleRate);
    const float seconds = std::max(releaseSeconds, 0.001F);
    // One-pole recovery toward unity. Fast enough that a single transient does
    // not duck the note behind it, slow enough not to pump on every pick.
    releaseCoefficient_ = 1.0F - std::exp(-1.0F / (seconds * rate));
    reset();
}

void SendLimiter::reset() noexcept {
    gain_ = 1.0F;
    deepestGain_ = 1.0F;
    limitedSamples_ = 0U;
}

void SendLimiter::process(std::span<float> samples) noexcept {
    for (float& sample : samples) {
        const float magnitude = std::fabs(sample);
        if (magnitude > threshold_) {
            // Computed from this sample and applied to this sample, so the
            // output cannot exceed the threshold even for one sample and
            // nothing has to be delayed to achieve it.
            const float required = threshold_ / magnitude;
            gain_ = std::min(gain_, required);
            deepestGain_ = std::min(deepestGain_, gain_);
            ++limitedSamples_;
        }
        if (gain_ < 1.0F) {
            sample *= gain_;
            // threshold / magnitude * magnitude is not exactly threshold in
            // binary floating point, so the multiply above can land one unit
            // in the last place over. Inaudible, but "never above the
            // threshold" is the single promise this class makes, and a
            // promise that holds to within a rounding error is a different
            // promise. Costs one compare on samples already at the ceiling.
            sample = std::clamp(sample, -threshold_, threshold_);
            // Recover toward unity. Held down only as long as it has to be.
            gain_ += (1.0F - gain_) * releaseCoefficient_;
            // Snapped well before unity, and not for tidiness.
            //
            // The step this recovery takes is proportional to the distance
            // remaining, so it shrinks as the gain approaches one. Close
            // enough and the step falls below the last place of a float near
            // one, the addition rounds to no change at all, and the gain
            // stalls a hair under unity -- permanently. Every session would
            // then run very slightly attenuated from its first loud note
            // onward, which nothing would ever report.
            //
            // Snapping here is four thousandths of a decibel, inaudible, and
            // safely above the value where the step stops registering.
            if (gain_ > 0.9995F) {
                gain_ = 1.0F;
            }
        }
        // When the gain is exactly one the sample is not touched at all, so a
        // block that never reaches the threshold comes out bit-identical.
    }
}

} // namespace jamlink::audio
