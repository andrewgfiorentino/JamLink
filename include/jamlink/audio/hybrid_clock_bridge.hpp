// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/async_mono_resampler.hpp"
#include "jamlink/audio/spsc_audio_ring.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jamlink::audio {

// Bridges one independently clocked mono capture producer into a master audio
// callback. The producer only writes the SPSC ring. The master callback owns
// the resampler, so its mutable control state never crosses threads.
class HybridClockBridge final {
public:
    HybridClockBridge(
        std::size_t ringCapacityFrames,
        std::size_t maximumIngressFrames,
        std::uint32_t destinationRate)
        : ring_(ringCapacityFrames, 1U),
          resampler_(ringCapacityFrames),
          ingress_(maximumIngressFrames, 0.0F),
          destinationRate_(destinationRate) {
        if (maximumIngressFrames == 0U) {
            throw std::invalid_argument("HybridClockBridge requires ingress storage");
        }
    }

    HybridClockBridge(const HybridClockBridge&) = delete;
    HybridClockBridge& operator=(const HybridClockBridge&) = delete;

    // Both threads must be stopped. Used before the first callback.
    void configureStopped(std::uint32_t sourceRate) {
        resampler_.configure(sourceRate, destinationRate_);
        ring_.clear();
        requestedRate_.store(sourceRate, std::memory_order_relaxed);
        const auto generation = requestedGeneration_.fetch_add(
            1U, std::memory_order_relaxed) + 1U;
        observedGeneration_ = generation;
        appliedGeneration_.store(generation, std::memory_order_release);
    }

    // Producer must be stopped. The consumer applies the reset in pull(); the
    // producer may resume after transitionApplied() reports the generation.
    [[nodiscard]] std::uint32_t requestSourceTransition(std::uint32_t sourceRate) noexcept {
        if (sourceRate < 8'000U || sourceRate > 384'000U) {
            return 0U;
        }
        requestedRate_.store(sourceRate, std::memory_order_relaxed);
        return requestedGeneration_.fetch_add(1U, std::memory_order_release) + 1U;
    }

    [[nodiscard]] bool transitionApplied(std::uint32_t generation) const noexcept {
        return generation != 0U
            && appliedGeneration_.load(std::memory_order_acquire) >= generation;
    }

    // Independent capture callback/worker. Allocation-free and lock-free.
    [[nodiscard]] std::size_t push(std::span<const float> monoSamples) noexcept {
        return ring_.write(monoSamples);
    }

    // Master callback. Allocation-free and lock-free.
    [[nodiscard]] std::size_t pull(std::span<float> destination) noexcept {
        const std::uint32_t requested = requestedGeneration_.load(std::memory_order_acquire);
        if (requested != observedGeneration_) {
            // The producer is stopped by contract while this reset occurs.
            ring_.clear();
            if (!resampler_.tryConfigure(
                    requestedRate_.load(std::memory_order_relaxed), destinationRate_)) {
                std::fill(destination.begin(), destination.end(), 0.0F);
                return 0U;
            }
            observedGeneration_ = requested;
            appliedGeneration_.store(requested, std::memory_order_release);
        }
        const std::size_t available = std::min(
            ring_.availableReadFrames(), ingress_.size());
        if (available > 0U) {
            const auto ingress = std::span<float>(ingress_.data(), available);
            static_cast<void>(ring_.readAndZeroFill(ingress));
            static_cast<void>(resampler_.write(ingress));
        }
        return resampler_.read(destination);
    }

    [[nodiscard]] std::size_t sourceOccupancyFrames() const noexcept {
        return ring_.availableReadFrames() + resampler_.availableFrames();
    }
    [[nodiscard]] std::uint64_t underrunCount() const noexcept {
        return ring_.underrunCount() + resampler_.underrunCount();
    }
    [[nodiscard]] std::uint64_t overrunCount() const noexcept {
        return ring_.overrunCount() + resampler_.overrunCount();
    }
    [[nodiscard]] double correctionPpm() const noexcept {
        return resampler_.lastCorrectionPpm();
    }

private:
    SpscAudioRing ring_;
    AsyncMonoResampler resampler_;
    std::vector<float> ingress_;
    const std::uint32_t destinationRate_;
    std::atomic<std::uint32_t> requestedRate_{48'000U};
    std::atomic<std::uint32_t> requestedGeneration_{0U};
    std::atomic<std::uint32_t> appliedGeneration_{0U};
    std::uint32_t observedGeneration_{0U};
};

} // namespace jamlink::audio
