// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace jamlink::audio {

// Bounded single-producer/single-consumer interleaved audio ring. Construction
// allocates on a control thread; read/write are allocation-free and lock-free.
class SpscAudioRing final {
public:
    SpscAudioRing(std::size_t capacityFrames, std::size_t channelCount)
        : capacityFrames_(validateShape(capacityFrames, channelCount)),
          channelCount_(channelCount),
          frameMask_(capacityFrames_ - 1U),
          samples_(capacityFrames_ * channelCount_, 0.0F) {}

    SpscAudioRing(const SpscAudioRing&) = delete;
    SpscAudioRing& operator=(const SpscAudioRing&) = delete;

    [[nodiscard]] std::size_t capacityFrames() const noexcept { return capacityFrames_; }
    [[nodiscard]] std::size_t channelCount() const noexcept { return channelCount_; }

    // Producer-thread API. Writes the complete block or nothing.
    [[nodiscard]] std::size_t write(std::span<const float> interleaved) noexcept {
        if (interleaved.size() % channelCount_ != 0U) {
            return 0U;
        }

        const std::size_t frames = interleaved.size() / channelCount_;
        const std::uint64_t writeFrame = writeCursor_.value.load(std::memory_order_relaxed);
        const std::uint64_t readFrame = readCursor_.value.load(std::memory_order_acquire);
        const auto usedFrames = static_cast<std::size_t>(writeFrame - readFrame);

        if (frames > capacityFrames_ - usedFrames) {
            overruns_.fetch_add(1U, std::memory_order_relaxed);
            return 0U;
        }

        copyIntoRing(writeFrame, interleaved, frames);
        writeCursor_.value.store(writeFrame + frames, std::memory_order_release);
        return frames;
    }

    // Consumer-thread API. Unavailable frames are deterministically zero-filled.
    [[nodiscard]] std::size_t readAndZeroFill(std::span<float> interleaved) noexcept {
        if (interleaved.size() % channelCount_ != 0U) {
            std::fill(interleaved.begin(), interleaved.end(), 0.0F);
            return 0U;
        }

        const std::size_t requestedFrames = interleaved.size() / channelCount_;
        const std::uint64_t readFrame = readCursor_.value.load(std::memory_order_relaxed);
        const std::uint64_t writeFrame = writeCursor_.value.load(std::memory_order_acquire);
        const auto available = static_cast<std::size_t>(writeFrame - readFrame);
        const std::size_t framesToRead = std::min(requestedFrames, available);

        copyFromRing(readFrame, interleaved.first(framesToRead * channelCount_), framesToRead);
        if (framesToRead < requestedFrames) {
            std::fill(
                interleaved.begin() + static_cast<std::ptrdiff_t>(framesToRead * channelCount_),
                interleaved.end(),
                0.0F);
            underruns_.fetch_add(1U, std::memory_order_relaxed);
        }

        readCursor_.value.store(readFrame + framesToRead, std::memory_order_release);
        return framesToRead;
    }

    [[nodiscard]] std::size_t availableReadFrames() const noexcept {
        const auto writeFrame = writeCursor_.value.load(std::memory_order_acquire);
        const auto readFrame = readCursor_.value.load(std::memory_order_acquire);
        return static_cast<std::size_t>(writeFrame - readFrame);
    }

    [[nodiscard]] std::size_t availableWriteFrames() const noexcept {
        return capacityFrames_ - availableReadFrames();
    }

    [[nodiscard]] std::uint64_t underrunCount() const noexcept {
        return underruns_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t overrunCount() const noexcept {
        return overruns_.load(std::memory_order_relaxed);
    }

    // Control-thread API; both producer and consumer must be stopped.
    void clear() noexcept {
        std::fill(samples_.begin(), samples_.end(), 0.0F);
        readCursor_.value.store(0U, std::memory_order_relaxed);
        writeCursor_.value.store(0U, std::memory_order_relaxed);
        underruns_.store(0U, std::memory_order_relaxed);
        overruns_.store(0U, std::memory_order_relaxed);
    }

private:
    [[nodiscard]] static std::size_t validateShape(
        std::size_t capacityFrames,
        std::size_t channelCount) {
        if (capacityFrames < 2U || (capacityFrames & (capacityFrames - 1U)) != 0U) {
            throw std::invalid_argument("SpscAudioRing capacity must be a power of two");
        }
        if (channelCount == 0U) {
            throw std::invalid_argument("SpscAudioRing requires at least one channel");
        }
        if (capacityFrames > std::numeric_limits<std::size_t>::max() / channelCount) {
            throw std::length_error("SpscAudioRing sample capacity overflows size_t");
        }
        return capacityFrames;
    }

    void copyIntoRing(
        std::uint64_t writeFrame,
        std::span<const float> source,
        std::size_t frames) noexcept {
        const std::size_t startFrame = static_cast<std::size_t>(writeFrame) & frameMask_;
        const std::size_t firstFrames = std::min(frames, capacityFrames_ - startFrame);
        const std::size_t firstSamples = firstFrames * channelCount_;
        std::memcpy(
            samples_.data() + startFrame * channelCount_,
            source.data(),
            firstSamples * sizeof(float));

        const std::size_t remainingSamples = source.size() - firstSamples;
        if (remainingSamples != 0U) {
            std::memcpy(
                samples_.data(),
                source.data() + firstSamples,
                remainingSamples * sizeof(float));
        }
    }

    void copyFromRing(
        std::uint64_t readFrame,
        std::span<float> destination,
        std::size_t frames) noexcept {
        const std::size_t startFrame = static_cast<std::size_t>(readFrame) & frameMask_;
        const std::size_t firstFrames = std::min(frames, capacityFrames_ - startFrame);
        const std::size_t firstSamples = firstFrames * channelCount_;
        std::memcpy(
            destination.data(),
            samples_.data() + startFrame * channelCount_,
            firstSamples * sizeof(float));

        const std::size_t remainingSamples = destination.size() - firstSamples;
        if (remainingSamples != 0U) {
            std::memcpy(
                destination.data() + firstSamples,
                samples_.data(),
                remainingSamples * sizeof(float));
        }
    }

    struct alignas(64) Cursor final {
        std::atomic<std::uint64_t> value{0};
        std::array<std::byte, 64U - sizeof(std::atomic<std::uint64_t>)> padding{};
    };

    static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
    static_assert(sizeof(Cursor) == 64U);

    Cursor writeCursor_;
    Cursor readCursor_;
    const std::size_t capacityFrames_;
    const std::size_t channelCount_;
    const std::size_t frameMask_;
    std::vector<float> samples_;
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> overruns_{0};
};

} // namespace jamlink::audio
