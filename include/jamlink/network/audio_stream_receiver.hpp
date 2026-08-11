// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jamlink::network {

struct AudioStreamReceiverTelemetry final {
    std::uint64_t packetsAccepted{0U};
    std::uint64_t packetsDuplicate{0U};
    std::uint64_t packetsReordered{0U};
    std::uint64_t packetsLate{0U};
    std::uint64_t packetsDropped{0U};
    std::uint64_t packetsConcealed{0U};
    std::uint64_t concealedFrames{0U};
    std::uint64_t bufferStretches{0U};
    std::uint64_t latencyTrims{0U};
    std::uint64_t resyncs{0U};
    std::uint64_t silentFrames{0U};
    std::uint32_t jitterMicroseconds{0U};
    std::uint32_t targetDepthFrames{0U};
    std::uint32_t currentDepthFrames{0U};
    bool playing{false};
};

struct AudioStreamReceiverSettings final {
    std::uint32_t sampleRate{48'000U};
    std::size_t packetFrames{240U};
    // Slot count must be a power of two and strictly greater than twice
    // maximumDepthPackets so that reordering never collides with playout.
    std::size_t slotCount{128U};
    std::size_t minimumDepthPackets{2U};
    std::size_t maximumDepthPackets{32U};
    // Receive depth targets jitter * safetyFactor plus one packet of headroom.
    double jitterSafetyFactor{2.5};
    // Packets the estimate must stay low before the target shrinks by one.
    std::uint32_t shrinkHoldPackets{400U};
};

// Adaptive receive jitter buffer with packet-loss concealment for one logical
// mono audio stream.
//
// Threading: single-producer/single-consumer. The network worker calls submit()
// and the audio callback calls pull(). Both are allocation-free and lock-free
// after construction. reset() is control-thread only and requires both sides to
// be stopped.
//
// The class holds no clock of its own: arrival times are supplied by the
// caller, which keeps behaviour exactly reproducible under virtual time.
class AudioStreamReceiver final {
public:
    explicit AudioStreamReceiver(const AudioStreamReceiverSettings& settings);

    AudioStreamReceiver(const AudioStreamReceiver&) = delete;
    AudioStreamReceiver& operator=(const AudioStreamReceiver&) = delete;

    // Network-worker side. Frames must hold exactly packetFrames() samples.
    void submit(
        std::uint32_t sequence,
        std::span<const float> frames,
        std::uint64_t arrivalMicroseconds) noexcept;

    // Audio-callback side. Always fills the destination completely: with real
    // audio, with concealment, or with silence before playout starts. Returns
    // the number of frames that carried live stream audio, so callers can tell
    // a running stream from a silent one without inspecting telemetry.
    [[nodiscard]] std::size_t pull(std::span<float> destination) noexcept;

    // Control thread only; the producer and consumer must both be stopped.
    void reset() noexcept;

    [[nodiscard]] std::size_t packetFrames() const noexcept { return packetFrames_; }

    // Total frames currently held between the network and the speaker, which is
    // the buffering latency this receiver is responsible for.
    [[nodiscard]] std::uint32_t bufferedFrames() const noexcept;

    [[nodiscard]] AudioStreamReceiverTelemetry telemetry() const noexcept;

private:
    struct Slot final {
        // Zero means empty. Otherwise the value is the stored sequence plus one.
        std::atomic<std::uint64_t> stamp{0U};
    };

    [[nodiscard]] static bool sequenceBefore(std::uint32_t left, std::uint32_t right) noexcept {
        return static_cast<std::int32_t>(left - right) < 0;
    }

    [[nodiscard]] float* slotSamples(std::size_t index) noexcept {
        return storage_.data() + index * packetFrames_;
    }

    // Consumer helpers.
    [[nodiscard]] bool beginPacket() noexcept;
    [[nodiscard]] bool tryPrime() noexcept;
    void takeRealPacket(std::size_t slotIndex) noexcept;
    // advancePlayout distinguishes covering a real gap from deliberately
    // stretching playout to rebuild the target backlog. decay says whether the
    // audio is genuinely missing, which is what fades the output out; a stretch
    // over a healthy stream must not fade.
    void concealPacket(bool advancePlayout, bool decay) noexcept;
    void blendRecoveryInto(std::span<float> packet) noexcept;
    void appendHistory(std::span<const float> block) noexcept;
    // Re-estimates the pitch period from contiguous real history, falling back
    // to the last good estimate when the tail is not clean enough to measure.
    [[nodiscard]] std::size_t refreshPeriod() noexcept;
    [[nodiscard]] float historyAt(std::uint64_t position) const noexcept {
        return history_[static_cast<std::size_t>(position) & historyMask_];
    }

    const std::uint32_t sampleRate_;
    const std::size_t packetFrames_;
    const std::size_t slotCount_;
    const std::size_t slotMask_;
    const std::size_t minimumDepthPackets_;
    const std::size_t maximumDepthPackets_;
    const double jitterSafetyFactor_;
    const std::uint32_t shrinkHoldPackets_;
    const double packetDurationMicroseconds_;

    std::vector<float> storage_;
    std::vector<Slot> slots_;

    // Consumer-owned playback state.
    std::vector<float> currentPacket_;
    std::vector<float> recoveryScratch_;
    // Untouched copy of a recovered packet's head, kept so the cross-fade never
    // contaminates the concealment history.
    std::vector<float> rawHead_;
    std::vector<float> history_;
    std::vector<float> fadeWindow_;
    std::size_t historyMask_;
    std::uint64_t historyWrite_{0U};
    std::size_t currentOffset_;
    std::uint32_t observedEpoch_{0U};
    std::size_t concealBurst_{0U};
    // Consecutive concealed packets that covered real loss. Drives the decay;
    // deliberate stretches deliberately do not.
    std::size_t lossBurst_{0U};
    std::size_t concealPeriod_{0U};
    std::size_t cachedPeriod_{0U};
    // Contiguous real frames at the end of the history, which bounds how far
    // back the period search may look.
    std::size_t realRunLength_{0U};
    // Frames synthesised since the last real audio, so a burst extrapolates
    // continuously instead of restarting the period every packet.
    std::size_t synthesisOffset_{0U};
    // Highest sequence observed on the previous playout packet, and how many
    // playout packets have passed without a new one. Together they say whether
    // the peer is still delivering.
    std::uint32_t previousHighest_{0U};
    std::size_t stalledPackets_{0U};
    bool recoveryPending_{false};

    // Producer-owned estimation state.
    std::uint32_t previousSequence_{0U};
    std::uint64_t previousArrival_{0U};
    bool hasPrevious_{false};
    double jitterMicroseconds_{0.0};
    std::size_t producerTarget_;
    std::uint32_t shrinkHold_{0U};

    // Cross-thread state.
    std::atomic<std::uint32_t> playoutSequence_{0U};
    std::atomic<std::uint32_t> highestSequence_{0U};
    std::atomic<std::uint32_t> firstSequence_{0U};
    std::atomic<std::uint32_t> resyncEpoch_{0U};
    std::atomic<bool> hasFirst_{false};
    std::atomic<bool> primed_{false};
    std::atomic<std::uint32_t> targetPackets_;
    std::atomic<std::uint32_t> jitterEstimate_{0U};

    std::atomic<std::uint64_t> packetsAccepted_{0U};
    std::atomic<std::uint64_t> packetsDuplicate_{0U};
    std::atomic<std::uint64_t> packetsReordered_{0U};
    std::atomic<std::uint64_t> packetsLate_{0U};
    std::atomic<std::uint64_t> packetsDropped_{0U};
    std::atomic<std::uint64_t> packetsConcealed_{0U};
    std::atomic<std::uint64_t> concealedFrames_{0U};
    std::atomic<std::uint64_t> bufferStretches_{0U};
    std::atomic<std::uint64_t> latencyTrims_{0U};
    std::atomic<std::uint64_t> resyncs_{0U};
    std::atomic<std::uint64_t> silentFrames_{0U};
};

} // namespace jamlink::network
