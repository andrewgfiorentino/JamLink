// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace jamlink::audio {

// A last resort between a hot input and a friend's headphones.
//
// Clipping is already detected and latched, and the musician is told. That is
// the right way round: the fix is at the interface's gain knob, and a
// processor that quietly cleaned it up would remove the reason to go and turn
// it down. But telling someone their input is too hot does not un-hurt the
// person who just heard it, and a guitarist who digs in on a chorus can
// overshoot for a bar without anything being wrong with their setup.
//
// So this sits on the network send only. What is monitored locally, what is
// recorded, and above all the pristine local originals are untouched: the
// originals are what was played and must stay lossless. This protects what
// leaves the machine.
//
// Deliberately no lookahead. Lookahead is how a mastering limiter avoids
// distortion, and it costs exactly as much latency as it buys quality --
// unacceptable on a path whose entire purpose is to be early. Instead the gain
// for a sample is computed from that same sample, which cannot overshoot at
// all, and only the recovery is smoothed.
class SendLimiter final {
public:
    // Just below full scale, so ordinary playing never reaches it and a real
    // over is caught before the encoder has to represent it.
    static constexpr float defaultThreshold = 0.944F;  // about -0.5 dBFS
    static constexpr float defaultReleaseSeconds = 0.050F;

    SendLimiter() noexcept = default;
    explicit SendLimiter(
        std::uint32_t sampleRate,
        float threshold = defaultThreshold,
        float releaseSeconds = defaultReleaseSeconds) noexcept;

    void prepare(
        std::uint32_t sampleRate,
        float threshold = defaultThreshold,
        float releaseSeconds = defaultReleaseSeconds) noexcept;

    // Realtime path: no allocation, no locking, no branching on denormals.
    //
    // A block that never reaches the threshold is returned bit-exactly. That is
    // not an accident of the arithmetic and is worth keeping: a limiter that
    // altered quiet playing would be colouring the instrument for nothing.
    void process(std::span<float> samples) noexcept;

    // How many samples the limiter has actually had to hold down. Zero is the
    // normal state and the number is diagnostic only -- it never replaces the
    // clip latch, which is what tells a musician to go and turn the gain down.
    [[nodiscard]] std::uint64_t limitedSamples() const noexcept { return limitedSamples_; }
    // The most this has had to pull the signal down, as a linear gain. One
    // means it has never engaged.
    [[nodiscard]] float deepestGain() const noexcept { return deepestGain_; }
    [[nodiscard]] bool engaged() const noexcept { return limitedSamples_ != 0U; }

    void reset() noexcept;

private:
    float threshold_{defaultThreshold};
    float releaseCoefficient_{0.0F};
    float gain_{1.0F};
    float deepestGain_{1.0F};
    std::uint64_t limitedSamples_{0U};
};

} // namespace jamlink::audio
