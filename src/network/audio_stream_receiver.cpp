// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/audio_stream_receiver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <numbers>
#include <stdexcept>

namespace jamlink::network {
namespace {

// Concealment history. Must exceed maximumLag + templateFrames so a full
// correlation search always has real samples to work with.
constexpr std::size_t historyFrames = 4'096U;
constexpr std::size_t templateFrames = 256U;
// 48 frames is about 1 kHz and 1200 frames is about 40 Hz at 48 kHz, which
// spans a low bass fundamental through the top of the guitar range.
constexpr std::size_t minimumLag = 48U;
constexpr std::size_t maximumLag = 1'200U;
constexpr std::size_t crossFadeFrames = 64U;
constexpr float decayPerPacket = 0.5F;
constexpr float minimumCorrelation = 0.25F;
constexpr float silenceEnergy = 1.0e-9F;
// Trim playout only once the backlog exceeds the target by this much, so normal
// jitter never provokes a correction.
constexpr std::int32_t trimSlackPackets = 2;
constexpr std::int32_t stretchSlackPackets = 1;
// Consecutive playout packets with no new arrival before a stream counts as
// stopped. The render callback can run several times per packet period, so a
// small count here is still only a few tens of milliseconds.
constexpr std::size_t stallLimitPackets = 4U;

[[nodiscard]] std::size_t validateSettings(const AudioStreamReceiverSettings& settings) {
    if (settings.packetFrames == 0U || settings.packetFrames > 4'096U) {
        throw std::invalid_argument("AudioStreamReceiver packetFrames is out of range");
    }
    if (settings.sampleRate < 8'000U || settings.sampleRate > 384'000U) {
        throw std::invalid_argument("AudioStreamReceiver sampleRate is out of range");
    }
    if (settings.slotCount < 4U
        || (settings.slotCount & (settings.slotCount - 1U)) != 0U) {
        throw std::invalid_argument("AudioStreamReceiver slotCount must be a power of two");
    }
    if (settings.minimumDepthPackets == 0U
        || settings.minimumDepthPackets > settings.maximumDepthPackets) {
        throw std::invalid_argument("AudioStreamReceiver depth bounds are inconsistent");
    }
    if (settings.maximumDepthPackets * 2U >= settings.slotCount) {
        throw std::invalid_argument(
            "AudioStreamReceiver slotCount must exceed twice maximumDepthPackets");
    }
    if (!(settings.jitterSafetyFactor >= 1.0) || !(settings.jitterSafetyFactor <= 16.0)) {
        throw std::invalid_argument("AudioStreamReceiver jitterSafetyFactor is out of range");
    }
    return settings.slotCount;
}

} // namespace

AudioStreamReceiver::AudioStreamReceiver(const AudioStreamReceiverSettings& settings)
    : AudioStreamReceiver(
          settings, std::make_unique<PcmPassThroughDecoder>(settings.packetFrames)) {}

AudioStreamReceiver::AudioStreamReceiver(
    const AudioStreamReceiverSettings& settings,
    std::unique_ptr<IAudioPacketDecoder> decoder)
    : sampleRate_(settings.sampleRate),
      packetFrames_(settings.packetFrames),
      slotCount_(validateSettings(settings)),
      slotMask_(slotCount_ - 1U),
      minimumDepthPackets_(settings.minimumDepthPackets),
      maximumDepthPackets_(settings.maximumDepthPackets),
      jitterSafetyFactor_(settings.jitterSafetyFactor),
      shrinkHoldPackets_(settings.shrinkHoldPackets),
      packetDurationMicroseconds_(
          static_cast<double>(settings.packetFrames) * 1'000'000.0
          / static_cast<double>(settings.sampleRate)),
      maximumPacketBytes_(
          decoder ? decoder->maximumPacketBytes() : settings.packetFrames * sizeof(float)),
      storage_(slotCount_ * maximumPacketBytes_, 0U),
      slots_(slotCount_),
      slotLengths_(slotCount_),
      decoder_(std::move(decoder)),
      currentPacket_(packetFrames_, 0.0F),
      recoveryScratchPacket_(packetFrames_, 0.0F),
      recoveryScratch_(crossFadeFrames, 0.0F),
      rawHead_(crossFadeFrames, 0.0F),
      history_(historyFrames, 0.0F),
      fadeWindow_(crossFadeFrames, 0.0F),
      historyMask_(historyFrames - 1U),
      currentOffset_(packetFrames_),
      producerTarget_(settings.minimumDepthPackets),
      targetPackets_(static_cast<std::uint32_t>(settings.minimumDepthPackets)) {
    for (std::size_t index = 0U; index < crossFadeFrames; ++index) {
        const double phase = std::numbers::pi * static_cast<double>(index)
            / static_cast<double>(crossFadeFrames);
        fadeWindow_[index] = static_cast<float>(0.5 * (1.0 - std::cos(phase)));
    }
}

void AudioStreamReceiver::submit(
    std::uint32_t sequence,
    std::span<const std::uint8_t> payload,
    std::uint64_t arrivalMicroseconds) noexcept {
    // Stored as it arrived and decoded at playout. Decoding here would put a
    // predictive codec's state in arrival order rather than sequence order, so
    // any reordering would corrupt it.
    if (payload.empty() || payload.size() > maximumPacketBytes_) {
        packetsDropped_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    if (!hasFirst_.load(std::memory_order_acquire)) {
        firstSequence_.store(sequence, std::memory_order_relaxed);
        highestSequence_.store(sequence, std::memory_order_relaxed);
        hasFirst_.store(true, std::memory_order_release);
    } else {
        const std::uint32_t highest = highestSequence_.load(std::memory_order_relaxed);
        const auto jump = static_cast<std::int32_t>(sequence - highest);
        if (jump > static_cast<std::int32_t>(slotCount_ / 2U)
            || jump < -static_cast<std::int32_t>(slotCount_ / 2U)) {
            // The peer restarted its numbering, or the stream stalled long
            // enough that the old backlog is worthless. Rebase and let the
            // consumer re-prime rather than concealing indefinitely.
            firstSequence_.store(sequence, std::memory_order_relaxed);
            highestSequence_.store(sequence, std::memory_order_relaxed);
            hasPrevious_ = false;
            resyncs_.fetch_add(1U, std::memory_order_relaxed);
            resyncEpoch_.fetch_add(1U, std::memory_order_release);
        } else if (jump > 0) {
            highestSequence_.store(sequence, std::memory_order_relaxed);
        }
    }

    if (hasPrevious_) {
        const auto step = static_cast<std::int32_t>(sequence - previousSequence_);
        if (step > 0 && step < static_cast<std::int32_t>(slotCount_ / 2U)) {
            const double expected = static_cast<double>(step) * packetDurationMicroseconds_;
            const double actual = static_cast<double>(arrivalMicroseconds)
                - static_cast<double>(previousArrival_);
            const double deviation = std::abs(actual - expected);
            // RFC 3550 style smoothing: cheap, allocation-free, and stable.
            jitterMicroseconds_ += (deviation - jitterMicroseconds_) / 16.0;
            previousSequence_ = sequence;
            previousArrival_ = arrivalMicroseconds;
        } else if (step >= static_cast<std::int32_t>(slotCount_ / 2U)) {
            // A gap this wide makes the expected arrival time meaningless, so
            // nothing is measured across it -- but the anchor has to move.
            //
            // Leaving it behind was a real defect. Resync rebases on a jump
            // measured from the highest sequence, while this guard measures
            // from the last packet used for timing, and there is a band where
            // one triggers and the other does not. A single gap of half the
            // ring -- about a third of a second, which is one Wi-Fi hiccup --
            // pinned this anchor permanently: every later packet then measured
            // an ever-larger step, stayed out of range forever, and the jitter
            // estimate froze. The buffer would never grow again however bad
            // the link became, which is silent, and audible only as dropouts
            // on a connection that had visibly got worse.
            previousSequence_ = sequence;
            previousArrival_ = arrivalMicroseconds;
        }
        // A step of zero or less is a reordered or duplicated packet. Neither
        // measured nor re-anchored: moving the anchor backwards would make the
        // next packet measure across a span that never happened.
    } else {
        previousSequence_ = sequence;
        previousArrival_ = arrivalMicroseconds;
        hasPrevious_ = true;
    }

    const double needed = jitterMicroseconds_ * jitterSafetyFactor_
        + packetDurationMicroseconds_;
    const auto desired = std::clamp(
        static_cast<std::size_t>(std::ceil(needed / packetDurationMicroseconds_)),
        minimumDepthPackets_,
        maximumDepthPackets_);
    if (desired > producerTarget_) {
        // Grow immediately: an underrun is far more audible than a few extra
        // milliseconds of buffer.
        producerTarget_ = desired;
        shrinkHold_ = 0U;
    } else if (desired < producerTarget_) {
        if (++shrinkHold_ >= shrinkHoldPackets_) {
            --producerTarget_;
            shrinkHold_ = 0U;
        }
    } else {
        shrinkHold_ = 0U;
    }
    targetPackets_.store(
        static_cast<std::uint32_t>(producerTarget_), std::memory_order_relaxed);
    jitterEstimate_.store(
        static_cast<std::uint32_t>(std::min(jitterMicroseconds_, 1'000'000.0)),
        std::memory_order_relaxed);

    const std::uint32_t playout = playoutSequence_.load(std::memory_order_acquire);
    const bool playing = primed_.load(std::memory_order_acquire);
    if (playing && sequenceBefore(sequence, playout)) {
        packetsLate_.fetch_add(1U, std::memory_order_relaxed);
        return;
    }

    const std::size_t slotIndex = static_cast<std::size_t>(sequence) & slotMask_;
    Slot& slot = slots_[slotIndex];
    const std::uint64_t stamp = slot.stamp.load(std::memory_order_acquire);
    if (stamp != 0U) {
        const auto existing = static_cast<std::uint32_t>(stamp - 1U);
        if (existing == sequence) {
            packetsDuplicate_.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        // Only a slot the consumer has already moved past may be reclaimed.
        if (!playing || !sequenceBefore(existing, playout)) {
            packetsDropped_.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
    }

    std::memcpy(slotBytes(slotIndex), payload.data(), payload.size());
    slotLengths_[slotIndex].store(
        static_cast<std::uint32_t>(payload.size()), std::memory_order_relaxed);
    // The stamp publishes the slot: its release orders the bytes and the length
    // written above ahead of any consumer that acquires it.
    slot.stamp.store(static_cast<std::uint64_t>(sequence) + 1U, std::memory_order_release);
    packetsAccepted_.fetch_add(1U, std::memory_order_relaxed);
    if (playing && sequenceBefore(sequence, highestSequence_.load(std::memory_order_relaxed))) {
        packetsReordered_.fetch_add(1U, std::memory_order_relaxed);
    }
}

std::size_t AudioStreamReceiver::pull(std::span<float> destination) noexcept {
    std::size_t written = 0U;
    std::size_t live = 0U;
    while (written < destination.size()) {
        if (currentOffset_ >= packetFrames_) {
            if (!beginPacket()) {
                const std::size_t remaining = destination.size() - written;
                std::fill_n(destination.begin() + static_cast<std::ptrdiff_t>(written),
                            remaining, 0.0F);
                silentFrames_.fetch_add(remaining, std::memory_order_relaxed);
                // Silence still enters the history so a stream that starts
                // mid-gap does not extrapolate from stale audio.
                appendHistory(std::span<const float>(
                    destination.data() + written, remaining));
                return live;
            }
            currentOffset_ = 0U;
        }
        const std::size_t chunk = std::min(
            destination.size() - written, packetFrames_ - currentOffset_);
        std::memcpy(
            destination.data() + written,
            currentPacket_.data() + currentOffset_,
            chunk * sizeof(float));
        currentOffset_ += chunk;
        written += chunk;
        live += chunk;
    }
    return live;
}

bool AudioStreamReceiver::beginPacket() noexcept {
    const std::uint32_t epoch = resyncEpoch_.load(std::memory_order_acquire);
    if (epoch != observedEpoch_) {
        observedEpoch_ = epoch;
        primed_.store(false, std::memory_order_release);
        concealBurst_ = 0U;
        lossBurst_ = 0U;
        recoveryPending_ = false;
    }

    if (!primed_.load(std::memory_order_acquire) && !tryPrime()) {
        return false;
    }

    const std::uint32_t target = targetPackets_.load(std::memory_order_relaxed);
    std::uint32_t playout = playoutSequence_.load(std::memory_order_relaxed);
    const std::uint32_t highest = highestSequence_.load(std::memory_order_acquire);
    const auto depth = static_cast<std::int32_t>(highest - playout) + 1;
    // The dead zone spans [target - stretchSlack, target + trimSlack]. Jitter
    // moves the observed depth by a packet or two every cycle, so correcting on
    // any deviation makes playout hunt: it inserts and removes audio several
    // times a second, which is audible as warble even when nothing is lost.
    // Whether the peer is still delivering. Depth cannot answer this: a stretch
    // freezes playout, so when a stream stops the depth freezes too and stays
    // wherever it was. Only the arrival of new sequences distinguishes "waiting
    // for the buffer to grow" from "this stream has gone away".
    if (highest != previousHighest_) {
        previousHighest_ = highest;
        stalledPackets_ = 0U;
    } else if (stalledPackets_ < stallLimitPackets) {
        ++stalledPackets_;
    }
    const bool arriving = stalledPackets_ < stallLimitPackets;

    if (depth <= 0 || depth < static_cast<std::int32_t>(target) - stretchSlackPackets) {
        // The target grew, or arrivals fell behind the playout clock. Emit one
        // concealed packet without consuming a sequence so the backlog rebuilds
        // towards the target. Without this the adaptive target is advisory only
        // and the buffer stays stuck at whatever depth it primed with.
        //
        // A stretch over a stream that is still delivering plays at full level.
        // A stretch over one that has stopped must fade out rather than
        // extrapolate the last note indefinitely.
        concealPacket(false, !arriving);
        bufferStretches_.fetch_add(1U, std::memory_order_relaxed);
        return true;
    }
    if (depth > static_cast<std::int32_t>(target) + trimSlackPackets) {
        // Latency crept above the target, usually after a burst of late
        // arrivals. Drop exactly one packet per call so the correction is
        // gradual and always cross-faded.
        const std::size_t staleIndex = static_cast<std::size_t>(playout) & slotMask_;
        Slot& stale = slots_[staleIndex];
        if (stale.stamp.load(std::memory_order_acquire)
            == static_cast<std::uint64_t>(playout) + 1U) {
            // Decoded and thrown away rather than simply skipped, so a
            // predictive codec does not fall a frame behind the encoder.
            discardDecoded(staleIndex);
            stale.stamp.store(0U, std::memory_order_release);
        }
        ++playout;
        playoutSequence_.store(playout, std::memory_order_release);
        latencyTrims_.fetch_add(1U, std::memory_order_relaxed);
        recoveryPending_ = true;
    }

    const std::size_t slotIndex = static_cast<std::size_t>(playout) & slotMask_;
    Slot& slot = slots_[slotIndex];
    if (slot.stamp.load(std::memory_order_acquire)
        == static_cast<std::uint64_t>(playout) + 1U) {
        if (!takeRealPacket(slotIndex)) {
            concealPacket(true, true);
        }
    } else {
        concealPacket(true, true);
    }
    return true;
}

bool AudioStreamReceiver::tryPrime() noexcept {
    if (!hasFirst_.load(std::memory_order_acquire)) {
        return false;
    }
    const std::uint32_t target = targetPackets_.load(std::memory_order_relaxed);
    const std::uint32_t highest = highestSequence_.load(std::memory_order_acquire);
    const std::uint32_t first = firstSequence_.load(std::memory_order_acquire);
    const auto span = static_cast<std::int32_t>(highest - first) + 1;
    if (span < static_cast<std::int32_t>(target)) {
        return false;
    }
    playoutSequence_.store(highest - (target - 1U), std::memory_order_release);
    primed_.store(true, std::memory_order_release);
    concealBurst_ = 0U;
    recoveryPending_ = true;
    return true;
}

bool AudioStreamReceiver::takeRealPacket(std::size_t slotIndex) noexcept {
    const std::size_t length = static_cast<std::size_t>(
        slotLengths_[slotIndex].load(std::memory_order_relaxed));
    const std::size_t produced = decoder_->decode(
        std::span<const std::uint8_t>(slotBytes(slotIndex), length),
        std::span<float>(currentPacket_));
    if (produced != packetFrames_) {
        // An undecodable packet is indistinguishable from a lost one at this
        // point, so release the slot and let the caller conceal. Playout is not
        // advanced here; concealPacket does that.
        slots_[slotIndex].stamp.store(0U, std::memory_order_release);
        packetsDropped_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }

    const std::uint32_t playout = playoutSequence_.load(std::memory_order_relaxed);
    // Publish the advance before releasing the slot so a duplicate arriving in
    // the gap is classified as late instead of re-filling a consumed slot.
    playoutSequence_.store(playout + 1U, std::memory_order_release);
    slots_[slotIndex].stamp.store(0U, std::memory_order_release);

    if (concealBurst_ != 0U || recoveryPending_) {
        // The cross-fade is an output-domain smoothing, not signal content. The
        // history keeps the untouched packet so a later concealment never
        // extrapolates a previous splice back into the audio.
        const std::size_t fade = std::min(crossFadeFrames, packetFrames_);
        std::memcpy(rawHead_.data(), currentPacket_.data(), fade * sizeof(float));
        blendRecoveryInto(currentPacket_);
        concealBurst_ = 0U;
        lossBurst_ = 0U;
        recoveryPending_ = false;
        appendHistory(std::span<const float>(rawHead_.data(), fade));
        appendHistory(std::span<const float>(
            currentPacket_.data() + fade, packetFrames_ - fade));
        return true;
    }
    appendHistory(currentPacket_);
    return true;
}

// Trimming latency throws a packet away. A predictive codec still has to see it
// or its state falls behind the encoder by exactly that frame, and every packet
// after it decodes from the wrong history.
void AudioStreamReceiver::discardDecoded(std::size_t slotIndex) noexcept {
    const std::size_t length = static_cast<std::size_t>(
        slotLengths_[slotIndex].load(std::memory_order_relaxed));
    static_cast<void>(decoder_->decode(
        std::span<const std::uint8_t>(slotBytes(slotIndex), length),
        std::span<float>(recoveryScratchPacket_)));
}

void AudioStreamReceiver::concealPacket(bool advancePlayout, bool decay) noexcept {
    // A stretch is not a loss. Playout is being held back to rebuild the
    // buffer, and no packet went missing, so the decoder must not be told one
    // did: advancing its state past the encoder would corrupt every frame
    // after it. Only a real gap is reported.
    if (advancePlayout && decoder_->conceal(std::span<float>(currentPacket_)) == packetFrames_) {
        // The codec synthesised the gap from its own state, which is both
        // better informed than anything this class can do for its signal and
        // required to keep the two ends in step. Its decay is its own, so the
        // loss decay below is deliberately not applied on top.
        //
        // Synthesised audio still never enters the history: the period
        // estimator must not lock onto an insertion instead of the instrument.
        realRunLength_ = 0U;
        const std::uint32_t playoutNow = playoutSequence_.load(std::memory_order_relaxed);
        playoutSequence_.store(playoutNow + 1U, std::memory_order_release);
        packetsConcealed_.fetch_add(1U, std::memory_order_relaxed);
        if (decay) {
            ++lossBurst_;
        }
        ++concealBurst_;
        recoveryPending_ = true;
        concealedFrames_.fetch_add(packetFrames_, std::memory_order_relaxed);
        return;
    }

    const bool enteringBurst = concealBurst_ == 0U;
    if (enteringBurst) {
        concealPeriod_ = refreshPeriod();
        synthesisOffset_ = 0U;
    }

    // Only missing audio decays. A stretch over a healthy stream is a
    // deliberate time-stretch and plays at full level: sharing the loss decay
    // made a growing target fade a lossless stream towards silence, because the
    // target can grow by several packets and each consecutive stretch would
    // halve the output again.
    const float startGain = decay
        ? std::pow(decayPerPacket, static_cast<float>(lossBurst_))
        : 1.0F;
    const float endGain = decay ? startGain * decayPerPacket : 1.0F;
    const auto frames = static_cast<float>(packetFrames_);

    for (std::size_t index = 0U; index < packetFrames_; ++index) {
        const float position = static_cast<float>(index) / frames;
        const float gain = startGain + (endGain - startGain) * position;
        float reference = 0.0F;
        if (concealPeriod_ != 0U) {
            // Repeat the last real period. Synthesised audio deliberately never
            // enters the history: feeding it back would let the estimator lock
            // onto its own insertion instead of the instrument's pitch.
            const std::size_t offset = synthesisOffset_ % concealPeriod_;
            reference = historyAt(historyWrite_ - concealPeriod_ + offset);
        } else if (historyWrite_ != 0U) {
            // No usable periodicity: ramp what was playing down to zero rather
            // than inventing a texture.
            reference = historyAt(historyWrite_ - 1U) * (1.0F - position);
        }
        ++synthesisOffset_;
        currentPacket_[index] = reference * gain;
    }

    if (enteringBurst && concealPeriod_ != 0U && historyWrite_ != 0U) {
        // The best-correlating period still leaves a small step at the splice.
        // Carrying the residual across and letting it decay makes the entry
        // exactly continuous without colouring the rest of the packet.
        const float residual = historyAt(historyWrite_ - 1U) - currentPacket_[0];
        const std::size_t fade = std::min(crossFadeFrames, packetFrames_);
        for (std::size_t index = 0U; index < fade; ++index) {
            // The residual can be nearly two full amplitudes when the chosen
            // period lands anti-phase, so bound the sum. Concealment must never
            // be louder than the audio it stands in for.
            currentPacket_[index] = std::clamp(
                currentPacket_[index] + residual * (1.0F - fadeWindow_[index]),
                -1.0F, 1.0F);
        }
    }
    realRunLength_ = 0U;

    if (advancePlayout) {
        const std::uint32_t playout = playoutSequence_.load(std::memory_order_relaxed);
        playoutSequence_.store(playout + 1U, std::memory_order_release);
        // Only a real gap counts as concealed loss; a deliberate stretch is
        // reported separately so telemetry never overstates network damage.
        packetsConcealed_.fetch_add(1U, std::memory_order_relaxed);
    }
    if (decay) {
        ++lossBurst_;
    }
    ++concealBurst_;
    recoveryPending_ = true;
    concealedFrames_.fetch_add(packetFrames_, std::memory_order_relaxed);
}

void AudioStreamReceiver::blendRecoveryInto(std::span<float> packet) noexcept {
    const std::size_t fade = std::min(crossFadeFrames, packet.size());
    if (fade == 0U) {
        return;
    }

    if (concealBurst_ == 0U) {
        // Priming and latency trims splice the signal too, but they can reuse
        // the last measured period. Running the full correlation search here
        // would put it in the render callback on every trim, which is once per
        // packet whenever the buffer sits above target.
        concealPeriod_ = cachedPeriod_;
        synthesisOffset_ = 0U;
    }

    // Continue the extrapolation the listener was already hearing, then raise
    // the real packet over it so recovery does not click.
    const float gain = lossBurst_ == 0U
        ? 1.0F
        : std::pow(decayPerPacket, static_cast<float>(lossBurst_));
    for (std::size_t index = 0U; index < fade; ++index) {
        float continued = 0.0F;
        if (concealPeriod_ != 0U) {
            const std::size_t offset = (synthesisOffset_ + index) % concealPeriod_;
            continued = historyAt(historyWrite_ - concealPeriod_ + offset) * gain;
        } else if (historyWrite_ != 0U) {
            continued = historyAt(historyWrite_ - 1U)
                * gain * (1.0F - static_cast<float>(index) / static_cast<float>(fade));
        }
        recoveryScratch_[index] = continued;
    }
    for (std::size_t index = 0U; index < fade; ++index) {
        const float weight = fadeWindow_[index];
        packet[index] = recoveryScratch_[index] * (1.0F - weight) + packet[index] * weight;
    }
}

// Only genuine decoded audio is appended. Concealment reads this history but
// never writes to it, which keeps the period estimator honest.
void AudioStreamReceiver::appendHistory(std::span<const float> block) noexcept {
    for (const float sample : block) {
        history_[static_cast<std::size_t>(historyWrite_) & historyMask_] =
            std::isfinite(sample) ? sample : 0.0F;
        ++historyWrite_;
    }
    realRunLength_ = std::min(realRunLength_ + block.size(), historyFrames);
    synthesisOffset_ = 0U;
}

std::size_t AudioStreamReceiver::refreshPeriod() noexcept {
    // Search only inside the run of contiguous real audio at the end of the
    // history. Crossing an earlier splice would let the estimator measure the
    // splice rather than the instrument.
    const std::size_t usable = std::min(realRunLength_, historyFrames);
    if (usable < templateFrames + minimumLag) {
        return cachedPeriod_;
    }
    const std::size_t lagLimit = std::min(maximumLag, usable - templateFrames);
    if (lagLimit < minimumLag) {
        return cachedPeriod_;
    }

    const std::uint64_t end = historyWrite_;
    double templateEnergy = 0.0;
    for (std::size_t index = 0U; index < templateFrames; index += 2U) {
        const double sample = historyAt(end - templateFrames + index);
        templateEnergy += sample * sample;
    }
    if (templateEnergy < static_cast<double>(silenceEnergy)) {
        cachedPeriod_ = 0U;
        return 0U;
    }

    // Coarse search on a 2:1 decimated view keeps the worst-case cost of a
    // concealment start bounded; it runs once per loss burst, not per packet.
    double bestScore = 0.0;
    std::size_t bestLag = 0U;
    for (std::size_t lag = minimumLag; lag <= lagLimit; lag += 2U) {
        double correlation = 0.0;
        double energy = 0.0;
        for (std::size_t index = 0U; index < templateFrames; index += 2U) {
            const double current = historyAt(end - templateFrames + index);
            const double past = historyAt(end - templateFrames + index - lag);
            correlation += current * past;
            energy += past * past;
        }
        if (energy < static_cast<double>(silenceEnergy)) {
            continue;
        }
        const double score = correlation / std::sqrt(energy * templateEnergy);
        if (score > bestScore) {
            bestScore = score;
            bestLag = lag;
        }
    }
    if (bestLag == 0U || bestScore < static_cast<double>(minimumCorrelation)) {
        return cachedPeriod_;
    }

    // Refine at full resolution around the coarse winner.
    const std::size_t low = bestLag > minimumLag + 2U ? bestLag - 2U : minimumLag;
    const std::size_t high = std::min(bestLag + 2U, lagLimit);
    double refinedScore = 0.0;
    std::size_t refinedLag = bestLag;
    for (std::size_t lag = low; lag <= high; ++lag) {
        double correlation = 0.0;
        double energy = 0.0;
        for (std::size_t index = 0U; index < templateFrames; ++index) {
            const double current = historyAt(end - templateFrames + index);
            const double past = historyAt(end - templateFrames + index - lag);
            correlation += current * past;
            energy += past * past;
        }
        if (energy < static_cast<double>(silenceEnergy)) {
            continue;
        }
        const double score = correlation / std::sqrt(energy);
        if (score > refinedScore) {
            refinedScore = score;
            refinedLag = lag;
        }
    }
    cachedPeriod_ = refinedLag;
    return refinedLag;
}

std::uint32_t AudioStreamReceiver::bufferedFrames() const noexcept {
    if (!primed_.load(std::memory_order_acquire)) {
        return 0U;
    }
    const std::uint32_t playout = playoutSequence_.load(std::memory_order_acquire);
    const std::uint32_t highest = highestSequence_.load(std::memory_order_acquire);
    const auto depth = static_cast<std::int32_t>(highest - playout) + 1;
    if (depth <= 0) {
        return 0U;
    }
    return static_cast<std::uint32_t>(static_cast<std::size_t>(depth) * packetFrames_);
}

void AudioStreamReceiver::configureDepth(
    std::size_t minimumPackets,
    std::size_t maximumPackets,
    double jitterSafetyFactor) noexcept {
    if (minimumPackets == 0U || minimumPackets > maximumPackets
        || maximumPackets * 2U >= slotCount_
        || !(jitterSafetyFactor >= 1.0) || !(jitterSafetyFactor <= 16.0)) {
        return;
    }
    minimumDepthPackets_ = minimumPackets;
    maximumDepthPackets_ = maximumPackets;
    jitterSafetyFactor_ = jitterSafetyFactor;
    producerTarget_ = minimumPackets;
    targetPackets_.store(
        static_cast<std::uint32_t>(minimumPackets), std::memory_order_relaxed);
}

void AudioStreamReceiver::reset() noexcept {
    decoder_->reset();
    for (auto& length : slotLengths_) {
        length.store(0U, std::memory_order_relaxed);
    }
    for (Slot& slot : slots_) {
        slot.stamp.store(0U, std::memory_order_relaxed);
    }
    std::fill(storage_.begin(), storage_.end(), std::uint8_t{0});
    std::fill(currentPacket_.begin(), currentPacket_.end(), 0.0F);
    std::fill(history_.begin(), history_.end(), 0.0F);
    std::fill(recoveryScratch_.begin(), recoveryScratch_.end(), 0.0F);
    std::fill(rawHead_.begin(), rawHead_.end(), 0.0F);
    historyWrite_ = 0U;
    currentOffset_ = packetFrames_;
    observedEpoch_ = 0U;
    concealBurst_ = 0U;
    lossBurst_ = 0U;
    concealPeriod_ = 0U;
    cachedPeriod_ = 0U;
    realRunLength_ = 0U;
    synthesisOffset_ = 0U;
    previousHighest_ = 0U;
    stalledPackets_ = 0U;
    recoveryPending_ = false;
    previousSequence_ = 0U;
    previousArrival_ = 0U;
    hasPrevious_ = false;
    jitterMicroseconds_ = 0.0;
    producerTarget_ = minimumDepthPackets_;
    shrinkHold_ = 0U;
    playoutSequence_.store(0U, std::memory_order_relaxed);
    highestSequence_.store(0U, std::memory_order_relaxed);
    firstSequence_.store(0U, std::memory_order_relaxed);
    resyncEpoch_.store(0U, std::memory_order_relaxed);
    hasFirst_.store(false, std::memory_order_relaxed);
    primed_.store(false, std::memory_order_relaxed);
    targetPackets_.store(
        static_cast<std::uint32_t>(minimumDepthPackets_), std::memory_order_relaxed);
    jitterEstimate_.store(0U, std::memory_order_relaxed);
    packetsAccepted_.store(0U, std::memory_order_relaxed);
    packetsDuplicate_.store(0U, std::memory_order_relaxed);
    packetsReordered_.store(0U, std::memory_order_relaxed);
    packetsLate_.store(0U, std::memory_order_relaxed);
    packetsDropped_.store(0U, std::memory_order_relaxed);
    packetsConcealed_.store(0U, std::memory_order_relaxed);
    concealedFrames_.store(0U, std::memory_order_relaxed);
    bufferStretches_.store(0U, std::memory_order_relaxed);
    latencyTrims_.store(0U, std::memory_order_relaxed);
    resyncs_.store(0U, std::memory_order_relaxed);
    silentFrames_.store(0U, std::memory_order_relaxed);
}

AudioStreamReceiverTelemetry AudioStreamReceiver::telemetry() const noexcept {
    AudioStreamReceiverTelemetry snapshot;
    snapshot.packetsAccepted = packetsAccepted_.load(std::memory_order_relaxed);
    snapshot.packetsDuplicate = packetsDuplicate_.load(std::memory_order_relaxed);
    snapshot.packetsReordered = packetsReordered_.load(std::memory_order_relaxed);
    snapshot.packetsLate = packetsLate_.load(std::memory_order_relaxed);
    snapshot.packetsDropped = packetsDropped_.load(std::memory_order_relaxed);
    snapshot.packetsConcealed = packetsConcealed_.load(std::memory_order_relaxed);
    snapshot.concealedFrames = concealedFrames_.load(std::memory_order_relaxed);
    snapshot.bufferStretches = bufferStretches_.load(std::memory_order_relaxed);
    snapshot.latencyTrims = latencyTrims_.load(std::memory_order_relaxed);
    snapshot.resyncs = resyncs_.load(std::memory_order_relaxed);
    snapshot.silentFrames = silentFrames_.load(std::memory_order_relaxed);
    snapshot.jitterMicroseconds = jitterEstimate_.load(std::memory_order_relaxed);
    snapshot.targetDepthFrames = static_cast<std::uint32_t>(
        static_cast<std::size_t>(targetPackets_.load(std::memory_order_relaxed))
        * packetFrames_);
    snapshot.currentDepthFrames = bufferedFrames();
    snapshot.playing = primed_.load(std::memory_order_acquire);
    return snapshot;
}

} // namespace jamlink::network
