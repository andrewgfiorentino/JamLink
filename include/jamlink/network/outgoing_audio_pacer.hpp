// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/async_mono_resampler.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>

namespace jamlink::network {

// Releases captured audio onto the wire at the cadence the audio itself
// represents, converting from the capture device's rate to the network rate on
// the way.
//
// Two rules this exists to keep, each learned from a live session.
//
// Packets must not clump. A receiver measures the spacing of its sender's
// arrivals and cannot distinguish sender bursts from network jitter, so a
// backlog emitted back to back makes a flawless link look unstable: the first
// successful two-home session ran over a 4 ms round trip and still reported a
// 135 ms receive buffer, with concealment running almost continuously.
//
// Captured audio must not be stranded. The design that replaced those bursts
// capped catch-up at four packets per wake-up and then, if still behind,
// rebased the schedule forward to the current time. Rebasing abandons the
// deficit: the audio behind it stays in the converter, the backlog grows every
// wake-up the sender is late, and once the converter fills it discards in
// chunks that the listener hears as glitching. Falling behind is made up here
// by releasing sooner, never by moving the deadline.
//
// The schedule is therefore advanced by exactly one packet per packet sent,
// and the only audio ever thrown away is a backlog older than a live session
// can use, which is counted so the loss is never silent.
//
// Single-thread owned and allocation-free after construction. The clock is
// injected so the schedule can be tested without real time.
class OutgoingAudioPacer final {
public:
    struct Telemetry final {
        std::uint64_t packetsReleased{0U};
        std::uint64_t framesAccepted{0U};
        // Source frames deliberately discarded because the backlog had grown
        // past what a live session can use. Counted so a drop is never silent.
        std::uint64_t framesDiscarded{0U};
        // Times the converter had no full packet ready when one was due, which
        // is what a capture side that cannot keep up looks like from here.
        std::uint64_t starvedReleases{0U};
        std::size_t backlogFrames{0U};
    };

    OutgoingAudioPacer(
        std::size_t packetFrames,
        std::uint32_t networkSampleRate,
        std::size_t maximumBacklogFrames)
        : packetFrames_(packetFrames),
          networkSampleRate_(networkSampleRate),
          maximumBacklogFrames_(maximumBacklogFrames),
          packetIntervalMicroseconds_(
              static_cast<std::uint64_t>(packetFrames) * 1'000'000ULL / networkSampleRate),
          maximumDeficitMicroseconds_(
              static_cast<std::uint64_t>(maximumBacklogFrames) * 1'000'000ULL
              / networkSampleRate),
          converter_(converterCapacity(maximumBacklogFrames)),
          scratch_(packetFrames, 0.0F) {
        if (packetFrames == 0U || networkSampleRate < 8'000U
            || networkSampleRate > 384'000U || maximumBacklogFrames < packetFrames) {
            throw std::invalid_argument("OutgoingAudioPacer configuration is out of range");
        }
    }

    // Returns false for a rate the converter cannot honour, leaving the pacer
    // untouched so a nonsense value cannot silently stop the stream.
    [[nodiscard]] bool setSourceRate(std::uint32_t sourceRate) noexcept {
        if (sourceRate == sourceRate_) {
            return true;
        }
        if (!converter_.tryConfigure(sourceRate, networkSampleRate_)) {
            return false;
        }
        sourceRate_ = sourceRate;
        nextReleaseMicroseconds_ = 0U;
        return true;
    }

    void reset() noexcept {
        converter_.clear();
        nextReleaseMicroseconds_ = 0U;
        packetsReleased_ = 0U;
        framesAccepted_ = 0U;
        framesDiscarded_ = 0U;
        starvedReleases_ = 0U;
    }

    // Takes captured audio at the configured source rate.
    void accept(std::span<const float> capturedFrames) noexcept {
        if (capturedFrames.empty() || sourceRate_ == 0U) {
            return;
        }
        framesAccepted_ += capturedFrames.size();
        static_cast<void>(converter_.write(capturedFrames));
        discardBacklogBeyondLimit();
    }

    // Fills `packet` and returns true when a packet is due and available. Call
    // in a loop until it returns false; the loop is self-limiting because the
    // deadline advances by one packet each time and the converter runs dry.
    [[nodiscard]] bool release(
        std::uint64_t nowMicroseconds,
        std::span<float> packet) noexcept {
        if (packet.size() != packetFrames_ || sourceRate_ == 0U) {
            return false;
        }
        if (nextReleaseMicroseconds_ == 0U
            || nowMicroseconds + packetIntervalMicroseconds_ < nextReleaseMicroseconds_) {
            // First packet of the session, or a clock that moved backwards.
            nextReleaseMicroseconds_ = nowMicroseconds;
        }
        // A sender that has been idle or blocked must not accumulate an
        // unbounded run of missed deadlines and then release them all at once.
        // The deficit is bounded to the same span of audio the backlog is,
        // because that is the most that can be waiting to go out anyway.
        if (nextReleaseMicroseconds_ + maximumDeficitMicroseconds_ < nowMicroseconds) {
            nextReleaseMicroseconds_ = nowMicroseconds - maximumDeficitMicroseconds_;
        }
        if (nowMicroseconds < nextReleaseMicroseconds_) {
            drainStandingBacklog(nowMicroseconds);
            return false;
        }
        // Never attempt a read that cannot complete. A converter asked for more
        // than it holds fills what it can, marks itself unprimed, and discards
        // the remainder; the partial block is unusable, so the audio in it is
        // simply lost. With a wake-up coarser than a packet that happened on
        // every pass, which is a steady, silent leak rather than a one-off.
        if (converter_.availableFrames() < requiredBacklogFrames()) {
            ++starvedReleases_;
            return false;
        }
        const std::size_t backlogBefore = converter_.availableFrames();
        const bool filled = converter_.read(packet) == packet.size();
        countPrimingDiscard(backlogBefore);
        if (!filled) {
            // A packet was due and the capture side had not produced one. The
            // deadline deliberately stays where it is: the audio behind it is
            // still coming, and moving the deadline forward would abandon the
            // credit for it. Doing exactly that, once per wake-up, is what held
            // an earlier design a few per cent below the rate it owed.
            ++starvedReleases_;
            return false;
        }
        nextReleaseMicroseconds_ += packetIntervalMicroseconds_;
        ++packetsReleased_;
        return true;
    }

    [[nodiscard]] Telemetry telemetry() const noexcept {
        return Telemetry{
            packetsReleased_,
            framesAccepted_,
            framesDiscarded_,
            starvedReleases_,
            converter_.availableFrames()};
    }

    [[nodiscard]] std::size_t packetFrames() const noexcept { return packetFrames_; }
    [[nodiscard]] std::uint64_t packetIntervalMicroseconds() const noexcept {
        return packetIntervalMicroseconds_;
    }

private:
    // The converter must hold the whole permitted backlog plus a packet of
    // working room, rounded up to the power of two it requires.
    [[nodiscard]] static std::size_t converterCapacity(std::size_t maximumBacklogFrames) {
        std::size_t capacity = 64U;
        while (capacity < maximumBacklogFrames * 2U) {
            capacity *= 2U;
        }
        return capacity;
    }

    // Backlog still standing once everything due has gone out is delay the two
    // players will carry for the rest of the session, because nothing else will
    // ever remove it: a sender that was blocked for half a second would leave
    // them permanently that much further apart. Pull the schedule in slightly
    // so the excess drains over the next few seconds.
    //
    // This is deliberately judged when the release loop has finished for this
    // wake-up rather than between packets. A sender that wakes every 50 ms
    // legitimately holds 50 ms of capture in the moment before it sends, and
    // treating that as excess would make it run permanently fast.
    void drainStandingBacklog(std::uint64_t nowMicroseconds) noexcept {
        if (nowMicroseconds == lastDrainCheckMicroseconds_) {
            return;
        }
        lastDrainCheckMicroseconds_ = nowMicroseconds;
        if (converter_.availableFrames() <= requiredBacklogFrames() * 2U) {
            return;
        }
        const std::uint64_t pullIn = packetIntervalMicroseconds_ / 8U;
        nextReleaseMicroseconds_ =
            nextReleaseMicroseconds_ > pullIn ? nextReleaseMicroseconds_ - pullIn : 0U;
    }

    // What the converter must hold before a full packet can be drawn from it.
    // It primes to twice a packet's worth of fill and needs a frame of
    // lookahead to interpolate, so keeping that much in hand is also what stops
    // it dropping back to unprimed between packets.
    [[nodiscard]] std::size_t requiredBacklogFrames() const noexcept {
        const auto nominal = static_cast<std::size_t>(
            static_cast<std::uint64_t>(packetFrames_) * sourceRate_ / networkSampleRate_);
        return nominal * 2U + 4U;
    }

    // The converter re-primes by jumping its read cursor to a working fill,
    // which silently drops everything older. That is the right behaviour at the
    // start of a stream, where the alternative is transmitting stale audio, but
    // it is still audio that was captured and never sent, so it is counted.
    void countPrimingDiscard(std::size_t backlogBefore) noexcept {
        const std::size_t backlogAfter = converter_.availableFrames();
        if (backlogBefore <= backlogAfter) {
            return;
        }
        const std::size_t consumed = backlogBefore - backlogAfter;
        const auto expected = static_cast<std::size_t>(
            static_cast<std::uint64_t>(packetFrames_) * sourceRate_ / networkSampleRate_);
        // Two frames of slack for the converter's interpolation guard and for
        // the drift correction it applies to the step.
        if (consumed > expected + 2U) {
            framesDiscarded_ += consumed - expected;
        }
    }

    // Audio older than a live session can use is worthless: sending it would
    // delay everything behind it. Discard it here, where it can be counted,
    // rather than leaving the converter to overrun silently.
    void discardBacklogBeyondLimit() noexcept {
        if (converter_.availableFrames() <= maximumBacklogFrames_) {
            return;
        }
        // Trim back to a working fill rather than to the limit. A sender that
        // was blocked has a backlog of stale audio behind it, and releasing it
        // at the media rate would carry that delay for the rest of the session:
        // the players would stay a fifth of a second further apart than they
        // were before the stall, permanently. Dropping it once, and saying so,
        // is the honest trade in a live jam.
        const std::size_t target = requiredBacklogFrames() * 2U;
        while (converter_.availableFrames() > target) {
            const std::size_t before = converter_.availableFrames();
            static_cast<void>(converter_.read(
                std::span<float>(scratch_.data(), scratch_.size())));
            const std::size_t after = converter_.availableFrames();
            if (after >= before) {
                // No progress is possible; stop rather than spin.
                break;
            }
            framesDiscarded_ += before - after;
        }
    }

    const std::size_t packetFrames_;
    const std::uint32_t networkSampleRate_;
    const std::size_t maximumBacklogFrames_;
    const std::uint64_t packetIntervalMicroseconds_;
    // How far behind the schedule may fall before catch-up is capped, which
    // bounds the largest burst a recovering sender can produce.
    const std::uint64_t maximumDeficitMicroseconds_;
    audio::AsyncMonoResampler converter_;
    std::vector<float> scratch_;
    std::uint32_t sourceRate_{0U};
    // When the next packet may leave. Zero means unscheduled.
    std::uint64_t nextReleaseMicroseconds_{0U};
    // Drain is judged once per wake-up, not once per poll.
    std::uint64_t lastDrainCheckMicroseconds_{0U};
    std::uint64_t packetsReleased_{0U};
    std::uint64_t framesAccepted_{0U};
    std::uint64_t framesDiscarded_{0U};
    std::uint64_t starvedReleases_{0U};
};

} // namespace jamlink::network
