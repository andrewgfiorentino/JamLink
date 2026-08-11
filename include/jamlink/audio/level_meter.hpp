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
    float peakHoldLinear{0.0F};
    float peakHoldDbfs{-160.0F};
    bool clipped{false};
    std::uint64_t clipSampleCount{0};
    std::uint64_t clipEventCount{0};
    std::uint64_t latestClipSampleOffset{0};
    std::uint64_t invalidSampleCount{0};
};

class LevelMeter final {
public:
    // Native integer capture reaches a value just below 1.0 when its largest
    // positive code is converted to float. This threshold catches that code
    // without treating merely hot (-1 dBFS) signals as clipped.
    static constexpr float nativeInputClipThreshold = 0.9999F;
    static constexpr float internalClipThreshold = 1.0F;

    explicit LevelMeter(float clipThreshold = internalClipThreshold) noexcept
        : clipThreshold_(std::clamp(clipThreshold, 0.5F, 1.0F)) {}

    // Real-time API. A sequence guard publishes one coherent snapshot per block.
    void process(std::span<const float> samples) noexcept {
        const std::uint64_t requestedReset =
            resetRequestGeneration_.load(std::memory_order_acquire);
        if (requestedReset != observedResetGeneration_) {
            observedResetGeneration_ = requestedReset;
            peakHold_ = 0.0F;
            clipSampleTotal_ = 0U;
            clipEventTotal_ = 0U;
            latestClipSampleOffset_ = 0U;
            invalidSampleTotal_ = 0U;
            previousSampleClipped_ = false;
        }

        float peak = 0.0F;
        double sumSquares = 0.0;
        std::uint64_t invalidSamples = 0U;
        bool blockClipped = false;

        for (std::size_t index = 0U; index < samples.size(); ++index) {
            const float sample = samples[index];
            bool sampleClipped = false;
            if (!std::isfinite(sample)) {
                ++invalidSamples;
                sampleClipped = true;
            } else {
                const float magnitude = std::abs(sample);
                peak = std::max(peak, magnitude);
                const double wideSample = static_cast<double>(sample);
                sumSquares += wideSample * wideSample;
                sampleClipped = magnitude >= clipThreshold_;
            }

            if (sampleClipped) {
                blockClipped = true;
                ++clipSampleTotal_;
                latestClipSampleOffset_ = totalSamplesProcessed_ + index;
                if (!previousSampleClipped_) {
                    ++clipEventTotal_;
                }
            }
            previousSampleClipped_ = sampleClipped;
        }

        const float meanSquare = samples.empty()
            ? 0.0F
            : static_cast<float>(sumSquares / static_cast<double>(samples.size()));

        invalidSampleTotal_ += invalidSamples;
        totalSamplesProcessed_ += samples.size();
        peakHold_ = std::max(peakHold_, peak);

        sequence_.fetch_add(1U, std::memory_order_acq_rel);
        peak_.store(peak, std::memory_order_relaxed);
        meanSquare_.store(meanSquare, std::memory_order_relaxed);
        peakHoldPublished_.store(peakHold_, std::memory_order_relaxed);
        if (blockClipped) {
            // A generation per clipped block ensures Reset cannot hide an
            // overload that is still present, even when it is one continuous
            // run spanning several callbacks.
            clipGeneration_.fetch_add(1U, std::memory_order_relaxed);
        }
        publishedClipSamples_.store(clipSampleTotal_, std::memory_order_relaxed);
        publishedClipEvents_.store(clipEventTotal_, std::memory_order_relaxed);
        publishedLatestClipSample_.store(latestClipSampleOffset_, std::memory_order_relaxed);
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
            const float peakHold = peakHoldPublished_.load(std::memory_order_relaxed);
            const std::uint64_t clipGeneration =
                clipGeneration_.load(std::memory_order_relaxed);
            const std::uint64_t clipSamples =
                publishedClipSamples_.load(std::memory_order_relaxed);
            const std::uint64_t clipEvents =
                publishedClipEvents_.load(std::memory_order_relaxed);
            const std::uint64_t latestClipSample =
                publishedLatestClipSample_.load(std::memory_order_relaxed);
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
                    peakHold,
                    toDbfs(peakHold),
                    clipGeneration
                        != acknowledgedClipGeneration_.load(std::memory_order_acquire),
                    clipSamples,
                    clipEvents,
                    latestClipSample,
                    invalidSamples};
            }
        }
    }

    void clearClipLatch() noexcept {
        acknowledgedClipGeneration_.store(
            clipGeneration_.load(std::memory_order_acquire),
            std::memory_order_release);
        resetRequestGeneration_.fetch_add(1U, std::memory_order_release);
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
    RealtimeAtomicFloat peakHoldPublished_;
    std::atomic<std::uint32_t> sequence_{0};
    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    std::atomic<std::uint64_t> clipGeneration_{0};
    std::atomic<std::uint64_t> acknowledgedClipGeneration_{0};
    std::atomic<std::uint64_t> resetRequestGeneration_{0};
    std::atomic<std::uint64_t> publishedClipSamples_{0};
    std::atomic<std::uint64_t> publishedClipEvents_{0};
    std::atomic<std::uint64_t> publishedLatestClipSample_{0};
    std::atomic<std::uint64_t> publishedInvalidSamples_{0};
    const float clipThreshold_;
    float peakHold_{0.0F};
    std::uint64_t clipSampleTotal_{0};
    std::uint64_t clipEventTotal_{0};
    std::uint64_t latestClipSampleOffset_{0};
    std::uint64_t invalidSampleTotal_{0};
    std::uint64_t totalSamplesProcessed_{0};
    std::uint64_t observedResetGeneration_{0};
    bool previousSampleClipped_{false};
};

} // namespace jamlink::audio
