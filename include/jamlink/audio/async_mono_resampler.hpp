// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace jamlink::audio {

// A bounded, allocation-free-after-construction mono FIFO with streaming
// linear interpolation. It is intentionally single-thread owned: platform
// audio services write capture packets and read render packets from the same
// event-driven worker. Occupancy feedback absorbs small independent-device
// clock errors without discontinuous sample insertion or removal.
class AsyncMonoResampler final {
public:
    explicit AsyncMonoResampler(std::size_t capacityFrames)
        : capacityFrames_(validateCapacity(capacityFrames)),
          frameMask_(capacityFrames_ - 1U),
          samples_(capacityFrames_, 0.0F) {}

    void configure(std::uint32_t sourceRate, std::uint32_t destinationRate) {
        if (!tryConfigure(sourceRate, destinationRate)) {
            throw std::invalid_argument("AsyncMonoResampler rate is outside the supported range");
        }
    }

    [[nodiscard]] bool tryConfigure(
        std::uint32_t sourceRate,
        std::uint32_t destinationRate) noexcept {
        if (sourceRate < 8'000U || sourceRate > 384'000U
            || destinationRate < 8'000U || destinationRate > 384'000U) {
            return false;
        }
        sourceRate_ = sourceRate;
        destinationRate_ = destinationRate;
        clear();
        return true;
    }

    void clear() noexcept {
        // Cursor reset makes old storage unreachable. Avoid clearing the full
        // backing array so a device transition remains constant-time in the
        // master realtime callback.
        writeCursor_ = 0U;
        readCursor_ = 0.0;
        primed_ = false;
        underruns_ = 0U;
        overruns_ = 0U;
        lastCorrectionPpm_ = 0.0;
    }

    [[nodiscard]] std::size_t write(std::span<const float> source) noexcept {
        if (source.empty()) {
            return 0U;
        }
        if (source.size() >= capacityFrames_) {
            ++overruns_;
            return 0U;
        }

        if (readCursor_ > static_cast<double>(writeCursor_)) {
            readCursor_ = static_cast<double>(writeCursor_);
            primed_ = false;
        }
        const auto readFrame = static_cast<std::uint64_t>(std::floor(readCursor_));
        const auto used = static_cast<std::size_t>(writeCursor_ - readFrame);
        constexpr std::size_t interpolationGuard = 2U;
        const auto usableCapacity = capacityFrames_ - interpolationGuard;
        if (source.size() > usableCapacity - std::min(used, usableCapacity)) {
            const auto framesToDiscard = source.size()
                - (usableCapacity - std::min(used, usableCapacity));
            readCursor_ += static_cast<double>(framesToDiscard);
            ++overruns_;
            primed_ = false;
        }

        for (const float sample : source) {
            samples_[static_cast<std::size_t>(writeCursor_) & frameMask_] =
                std::isfinite(sample) ? sample : 0.0F;
            ++writeCursor_;
        }
        return source.size();
    }

    [[nodiscard]] std::size_t read(std::span<float> destination) noexcept {
        std::fill(destination.begin(), destination.end(), 0.0F);
        if (destination.empty() || sourceRate_ == 0U || destinationRate_ == 0U) {
            return 0U;
        }

        const double nominalStep = static_cast<double>(sourceRate_)
            / static_cast<double>(destinationRate_);
        const double targetFill = std::clamp(
            static_cast<double>(destination.size()) * nominalStep * 2.0,
            16.0,
            static_cast<double>(capacityFrames_ / 4U));
        double available = static_cast<double>(writeCursor_) - readCursor_;
        if (!primed_) {
            if (available < targetFill + 2.0) {
                ++underruns_;
                return 0U;
            }
            readCursor_ = static_cast<double>(writeCursor_) - targetFill;
            available = targetFill;
            primed_ = true;
        }

        const double normalizedError = (available - targetFill) / targetFill;
        lastCorrectionPpm_ = std::clamp(normalizedError * 1'000.0, -500.0, 500.0);
        const double step = nominalStep * (1.0 + lastCorrectionPpm_ * 1.0e-6);

        std::size_t produced = 0U;
        for (; produced < destination.size(); ++produced) {
            const auto leftFrame = static_cast<std::uint64_t>(std::floor(readCursor_));
            if (leftFrame + 1U >= writeCursor_) {
                primed_ = false;
                ++underruns_;
                break;
            }
            const float left = samples_[static_cast<std::size_t>(leftFrame) & frameMask_];
            const float right = samples_[static_cast<std::size_t>(leftFrame + 1U) & frameMask_];
            const float fraction = static_cast<float>(readCursor_ - static_cast<double>(leftFrame));
            destination[produced] = left + (right - left) * fraction;
            readCursor_ += step;
        }
        return produced;
    }

    [[nodiscard]] std::size_t capacityFrames() const noexcept { return capacityFrames_; }
    [[nodiscard]] std::size_t availableFrames() const noexcept {
        return static_cast<std::size_t>(
            std::max(0.0, static_cast<double>(writeCursor_) - readCursor_));
    }
    [[nodiscard]] std::uint64_t underrunCount() const noexcept { return underruns_; }
    [[nodiscard]] std::uint64_t overrunCount() const noexcept { return overruns_; }
    [[nodiscard]] double lastCorrectionPpm() const noexcept { return lastCorrectionPpm_; }

private:
    [[nodiscard]] static std::size_t validateCapacity(std::size_t capacityFrames) {
        if (capacityFrames < 64U
            || (capacityFrames & (capacityFrames - 1U)) != 0U) {
            throw std::invalid_argument(
                "AsyncMonoResampler capacity must be a power of two of at least 64 frames");
        }
        return capacityFrames;
    }

    const std::size_t capacityFrames_;
    const std::size_t frameMask_;
    std::vector<float> samples_;
    std::uint32_t sourceRate_{0U};
    std::uint32_t destinationRate_{0U};
    std::uint64_t writeCursor_{0U};
    double readCursor_{0.0};
    bool primed_{false};
    std::uint64_t underruns_{0U};
    std::uint64_t overruns_{0U};
    double lastCorrectionPpm_{0.0};
};

} // namespace jamlink::audio
