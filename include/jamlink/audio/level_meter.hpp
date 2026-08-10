// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/realtime_atomic.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <span>

namespace jamlink::audio {

struct LevelSnapshot final {
    float peakLinear{0.0F};
    float rmsLinear{0.0F};
    float peakDbfs{-160.0F};
    float rmsDbfs{-160.0F};
    bool clipped{false};
    std::uint64_t invalidSampleCount{0};
};

class LevelMeter final {
public:
    // Real-time API. A sequence guard publishes one coherent snapshot per block.
    void process(std::span<const float> samples) noexcept {
        float peak = 0.0F;
        double sumSquares = 0.0;
        std::uint64_t invalidSamples = 0U;

        for (const float sample : samples) {
            if (!std::isfinite(sample)) {
                ++invalidSamples;
                continue;
            }
            const float magnitude = std::abs(sample);
            peak = std::max(peak, magnitude);
            const double wideSample = static_cast<double>(sample);
            sumSquares += wideSample * wideSample;
        }

        const float meanSquare = samples.empty()
            ? 0.0F
            : static_cast<float>(sumSquares / static_cast<double>(samples.size()));

        invalidSampleTotal_ += invalidSamples;

        sequence_.fetch_add(1U, std::memory_order_acq_rel);
        peak_.store(peak, std::memory_order_relaxed);
        meanSquare_.store(meanSquare, std::memory_order_relaxed);
        if (peak >= 1.0F) {
            clipGeneration_.fetch_add(1U, std::memory_order_relaxed);
        }
        publishedInvalidSamples_.store(invalidSampleTotal_, std::memory_order_relaxed);
        sequence_.fetch_add(1U, std::memory_order_release);
    }

    // Control/UI-thread API.
    [[nodiscard]] LevelSnapshot snapshot() const noexcept {
        for (;;) {
            const std::uint32_t sequenceBefore = sequence_.load(std::memory_order_acquire);
            if ((sequenceBefore & 1U) != 0U) {
                continue;
            }

            const float peak = peak_.load(std::memory_order_relaxed);
            const float meanSquare = meanSquare_.load(std::memory_order_relaxed);
            const std::uint64_t clipGeneration =
                clipGeneration_.load(std::memory_order_relaxed);
            const std::uint64_t invalidSamples =
                publishedInvalidSamples_.load(std::memory_order_relaxed);
            const std::uint32_t sequenceAfter = sequence_.load(std::memory_order_acquire);
            if (sequenceBefore == sequenceAfter) {
                const float rms = std::sqrt(std::max(0.0F, meanSquare));
                return LevelSnapshot{
                    peak,
                    rms,
                    toDbfs(peak),
                    toDbfs(rms),
                    clipGeneration
                        != acknowledgedClipGeneration_.load(std::memory_order_acquire),
                    invalidSamples};
            }
        }
    }

    void clearClipLatch() noexcept {
        acknowledgedClipGeneration_.store(
            clipGeneration_.load(std::memory_order_acquire),
            std::memory_order_release);
    }

private:
    [[nodiscard]] static float toDbfs(float linear) noexcept {
        constexpr float floorDb = -160.0F;
        if (linear <= 1.0e-8F) {
            return floorDb;
        }
        return std::max(floorDb, 20.0F * std::log10(linear));
    }

    RealtimeAtomicFloat peak_;
    RealtimeAtomicFloat meanSquare_;
    std::atomic<std::uint32_t> sequence_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> clipGeneration_{0};
    std::atomic<std::uint64_t> acknowledgedClipGeneration_{0};
    std::atomic<std::uint64_t> publishedInvalidSamples_{0};
    std::uint64_t invalidSampleTotal_{0};
};

} // namespace jamlink::audio
