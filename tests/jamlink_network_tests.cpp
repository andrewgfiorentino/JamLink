// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/audio_stream_receiver.hpp"
#include "support/packet_impairment.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::atomic<bool> allocationTrackingEnabled{false};
std::atomic<std::size_t> trackedAllocationCount{0};

} // namespace

void* operator new(std::size_t size) {
    if (allocationTrackingEnabled.load(std::memory_order_relaxed)) {
        trackedAllocationCount.fetch_add(1U, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

namespace {

using TestFunction = std::function<void()>;

struct TestCase final {
    std::string name;
    TestFunction function;
};

std::vector<TestCase>& tests() {
    static std::vector<TestCase> allTests;
    return allTests;
}

struct RegisterTest final {
    RegisterTest(std::string name, TestFunction function) {
        tests().push_back(TestCase{std::move(name), std::move(function)});
    }
};

[[noreturn]] void fail(const char* expression, const char* file, int line) {
    throw std::runtime_error(
        std::string(file) + ':' + std::to_string(line) + " expectation failed: " + expression);
}

#define JAMLINK_TEST(name) \
    void name(); \
    const RegisterTest register_##name(#name, name); \
    void name()

#define EXPECT_TRUE(expression) \
    do { if (!(expression)) { fail(#expression, __FILE__, __LINE__); } } while (false)

using jamlink::network::AudioStreamReceiver;
using jamlink::network::AudioStreamReceiverSettings;

constexpr std::size_t packetFrames = 240U;
constexpr std::uint32_t sampleRate = 48'000U;
constexpr std::uint64_t packetPeriodMicroseconds = 5'000U;
// Mirrors the receiver's recovery cross-fade length. Blocks spliced after a
// gap or a trim are smoothed over this many frames.
constexpr std::size_t recoveryCrossFadeFrames = 64U;

[[nodiscard]] AudioStreamReceiverSettings defaultSettings() {
    AudioStreamReceiverSettings settings;
    settings.sampleRate = sampleRate;
    settings.packetFrames = packetFrames;
    settings.slotCount = 128U;
    settings.minimumDepthPackets = 2U;
    settings.maximumDepthPackets = 32U;
    return settings;
}

// A plucked-string-like tone: a non-integer period at 48 kHz with several
// harmonics, which is far more representative of an instrument feed than a
// single sine at an exact bin.
[[nodiscard]] std::vector<float> makeSource(std::size_t frames, double fundamental = 110.0) {
    std::vector<float> signal(frames, 0.0F);
    for (std::size_t index = 0U; index < frames; ++index) {
        const double time = static_cast<double>(index) / static_cast<double>(sampleRate);
        const double phase = 2.0 * std::numbers::pi * fundamental * time;
        const double value = 0.50 * std::sin(phase)
            + 0.25 * std::sin(2.0 * phase)
            + 0.15 * std::sin(3.0 * phase)
            + 0.08 * std::sin(5.0 * phase);
        signal[index] = static_cast<float>(value * 0.7);
    }
    return signal;
}

[[nodiscard]] double energy(std::span<const float> block) {
    double total = 0.0;
    for (const float sample : block) {
        total += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return total;
}

[[nodiscard]] double errorEnergy(std::span<const float> left, std::span<const float> right) {
    double total = 0.0;
    for (std::size_t index = 0U; index < left.size(); ++index) {
        const double difference =
            static_cast<double>(left[index]) - static_cast<double>(right[index]);
        total += difference * difference;
    }
    return total;
}

[[nodiscard]] float maximumStep(std::span<const float> block) {
    float worst = 0.0F;
    for (std::size_t index = 1U; index < block.size(); ++index) {
        worst = std::max(worst, std::abs(block[index] - block[index - 1U]));
    }
    return worst;
}

[[nodiscard]] bool allFinite(std::span<const float> block) {
    return std::all_of(block.begin(), block.end(), [](float sample) {
        return std::isfinite(sample) && std::abs(sample) <= 4.0F;
    });
}

// Drives the receiver one packet at a time on an exact cadence. Packet indices
// listed in lostPackets are never submitted. Output block i corresponds to
// source packet i - 1 because priming consumes one extra cadence step.
enum class BlockKind : std::uint8_t { Silence, Real, ConcealedLoss, Stretch };

struct PacketRun final {
    std::vector<float> output;
    std::vector<BlockKind> kinds;
    // The source packet each block was responsible for. Stretch blocks are
    // inserted audio and carry the sequence that is still pending.
    std::vector<std::size_t> sequences;

    [[nodiscard]] std::span<const float> block(std::size_t index) const {
        return std::span<const float>(output.data() + index * packetFrames, packetFrames);
    }

    // Blocks are not a fixed offset from source packets: a stretch inserts a
    // block without consuming one, so tests must look the position up.
    [[nodiscard]] std::size_t findBlock(BlockKind kind, std::size_t sequence) const {
        for (std::size_t index = 0U; index < kinds.size(); ++index) {
            if (kinds[index] == kind && sequences[index] == sequence) {
                return index;
            }
        }
        return kinds.size();
    }
};

[[nodiscard]] PacketRun runPacketised(
    AudioStreamReceiver& receiver,
    std::span<const float> source,
    const std::vector<std::size_t>& lostPackets) {
    const std::size_t packetCount = source.size() / packetFrames;
    PacketRun run;
    run.output.assign(packetCount * packetFrames, 0.0F);
    run.kinds.assign(packetCount, BlockKind::Silence);
    run.sequences.assign(packetCount, 0U);

    std::uint64_t now = 0U;
    std::size_t playout = 0U;
    auto previous = receiver.telemetry();
    for (std::size_t packet = 0U; packet < packetCount; ++packet) {
        const bool lost = std::find(lostPackets.begin(), lostPackets.end(), packet)
            != lostPackets.end();
        if (!lost) {
            receiver.submit(
                static_cast<std::uint32_t>(packet),
                source.subspan(packet * packetFrames, packetFrames),
                now);
        }
        now += packetPeriodMicroseconds;
        const std::size_t live = receiver.pull(
            std::span<float>(run.output.data() + packet * packetFrames, packetFrames));

        const auto current = receiver.telemetry();
        // A trim discards one queued packet before producing this block.
        playout += static_cast<std::size_t>(current.latencyTrims - previous.latencyTrims);
        run.sequences[packet] = playout;
        if (live == 0U) {
            run.kinds[packet] = BlockKind::Silence;
        } else if (current.packetsConcealed != previous.packetsConcealed) {
            run.kinds[packet] = BlockKind::ConcealedLoss;
            ++playout;
        } else if (current.bufferStretches != previous.bufferStretches) {
            run.kinds[packet] = BlockKind::Stretch;
        } else {
            run.kinds[packet] = BlockKind::Real;
            ++playout;
        }
        previous = current;
    }
    return run;
}

JAMLINK_TEST(receiver_delivers_clean_stream_bit_exactly) {
    AudioStreamReceiver receiver(defaultSettings());
    const auto source = makeSource(packetFrames * 40U);
    const auto run = runPacketised(receiver, source, {});

    // The first audible block is faded in from priming silence so the stream
    // never starts on a click; every later block must be untouched.
    const std::size_t first = run.findBlock(BlockKind::Real, 0U);
    EXPECT_TRUE(first < run.kinds.size());
    EXPECT_TRUE(std::abs(run.block(first)[0]) < 1.0e-6F);
    for (std::size_t index = first + 1U; index < run.kinds.size(); ++index) {
        EXPECT_TRUE(run.kinds[index] == BlockKind::Real);
        const auto produced = run.block(index);
        const auto expected = std::span<const float>(
            source.data() + run.sequences[index] * packetFrames, packetFrames);
        for (std::size_t sample = 0U; sample < packetFrames; ++sample) {
            EXPECT_TRUE(produced[sample] == expected[sample]);
        }
    }

    const auto telemetry = receiver.telemetry();
    EXPECT_TRUE(telemetry.packetsConcealed == 0U);
    EXPECT_TRUE(telemetry.packetsLate == 0U);
    EXPECT_TRUE(telemetry.packetsDropped == 0U);
    EXPECT_TRUE(telemetry.latencyTrims == 0U);
    // A perfectly paced stream must neither stretch nor trim.
    EXPECT_TRUE(telemetry.bufferStretches == 0U);
    EXPECT_TRUE(telemetry.playing);
}

JAMLINK_TEST(concealment_beats_zero_fill_on_isolated_loss) {
    const auto source = makeSource(packetFrames * 40U);
    const std::vector<std::size_t> lost{20U};

    AudioStreamReceiver receiver(defaultSettings());
    const auto run = runPacketised(receiver, source, lost);

    const std::size_t gap = run.findBlock(BlockKind::ConcealedLoss, 20U);
    EXPECT_TRUE(gap < run.kinds.size());
    const auto produced = run.block(gap);
    const auto truth = std::span<const float>(
        source.data() + 20U * packetFrames, packetFrames);

    const double plcError = errorEnergy(produced, truth);
    const double zeroFillError = energy(truth);
    EXPECT_TRUE(zeroFillError > 0.0);

    std::cout << "    isolated loss: concealment error "
              << (10.0 * std::log10(plcError / zeroFillError))
              << " dB relative to zero fill\n";

    // Zero-fill error is exactly the signal energy, so any real concealment
    // must land well under it. Requiring a 6 dB improvement keeps the bar
    // meaningful without encoding one particular estimator's exact output.
    EXPECT_TRUE(plcError < zeroFillError * 0.25);
    EXPECT_TRUE(allFinite(produced));

    // The splice must not introduce a step far beyond the signal's own slope.
    const auto spliceRegion = std::span<const float>(
        run.output.data() + gap * packetFrames - 8U, packetFrames + 16U);
    const auto sourceRegion = std::span<const float>(
        source.data() + 20U * packetFrames, packetFrames);
    const float stepRatio = maximumStep(spliceRegion) / maximumStep(sourceRegion);
    std::size_t worstAt = 0U;
    float worstStep = 0.0F;
    for (std::size_t index = 1U; index < spliceRegion.size(); ++index) {
        const float step = std::abs(spliceRegion[index] - spliceRegion[index - 1U]);
        if (step > worstStep) {
            worstStep = step;
            worstAt = index;
        }
    }
    std::cout << "    isolated loss: worst splice step is " << stepRatio
              << "x the signal's own maximum step, at offset "
              << (static_cast<std::ptrdiff_t>(worstAt) - 8) << " within the gap\n";
    // Concealment must not inject a discontinuity the source never had.
    EXPECT_TRUE(stepRatio < 1.5F);

    const auto telemetry = receiver.telemetry();
    // Exactly one packet of real loss. The receiver also stretches once while
    // it waits for the missing packet, so synthesised frames exceed the gap.
    EXPECT_TRUE(telemetry.packetsConcealed == 1U);
    EXPECT_TRUE(telemetry.concealedFrames >= packetFrames);
    EXPECT_TRUE(telemetry.packetsLate == 0U);
}

JAMLINK_TEST(concealment_decays_to_silence_under_burst_loss) {
    const auto source = makeSource(packetFrames * 40U);
    const std::vector<std::size_t> lost{20U, 21U, 22U, 23U, 24U, 25U, 26U, 27U};

    AudioStreamReceiver receiver(defaultSettings());
    const auto run = runPacketised(receiver, source, lost);

    EXPECT_TRUE(allFinite(run.output));

    // A burst is covered by a mix of stretches (while the receiver waits for
    // packets that may still arrive) and loss concealment (once it gives up),
    // so the gap is located by scanning for the run of non-real blocks rather
    // than assuming a fixed layout.
    std::size_t firstGap = run.kinds.size();
    std::size_t lastGap = run.kinds.size();
    for (std::size_t index = 0U; index < run.kinds.size(); ++index) {
        const bool synthetic = run.kinds[index] == BlockKind::ConcealedLoss
            || run.kinds[index] == BlockKind::Stretch;
        if (!synthetic) {
            continue;
        }
        if (firstGap == run.kinds.size()) {
            firstGap = index;
        }
        lastGap = index;
    }
    EXPECT_TRUE(firstGap < run.kinds.size() && lastGap > firstGap);
    std::cout << "    burst loss: covered by " << (lastGap - firstGap + 1U)
              << " synthetic blocks\n";
    const auto firstConcealed = run.block(firstGap);
    const auto lastConcealed = run.block(lastGap);

    // A long dropout must fade out rather than hold a synthetic drone.
    EXPECT_TRUE(energy(lastConcealed) < energy(firstConcealed) * 0.01);
    std::cout << "    burst loss: 8 packet gap decayed by "
              << (10.0 * std::log10(
                     std::max(energy(lastConcealed), 1.0e-30)
                     / std::max(energy(firstConcealed), 1.0e-30)))
              << " dB\n";

    // Recovery must return to the real signal. The first real block after the
    // gap is cross-faded, so exactness is checked from the one after it.
    // While playout catches back up, each latency trim cross-fades the head of
    // the packet it splices to. Everything past that cross-fade must be the
    // untouched source.
    std::size_t checked = 0U;
    for (std::size_t index = lastGap + 2U; index < run.kinds.size(); ++index) {
        EXPECT_TRUE(run.kinds[index] == BlockKind::Real);
        const auto produced = run.block(index);
        const auto truth = std::span<const float>(
            source.data() + run.sequences[index] * packetFrames, packetFrames);
        for (std::size_t sample = recoveryCrossFadeFrames; sample < packetFrames; ++sample) {
            EXPECT_TRUE(produced[sample] == truth[sample]);
        }
        ++checked;
    }
    EXPECT_TRUE(checked > 0U);

    // Playout must catch back up rather than carrying the dropout as permanent
    // added latency.
    const auto telemetry = receiver.telemetry();
    EXPECT_TRUE(telemetry.currentDepthFrames
        <= 4U * static_cast<std::uint32_t>(packetFrames));
}

JAMLINK_TEST(receiver_recovers_reordered_packets_instead_of_dropping_them) {
    AudioStreamReceiver receiver(defaultSettings());
    const auto source = makeSource(packetFrames * 24U);

    // Deliver a run of packets with 10 and 11 swapped on the wire. The old
    // monotonic-sequence rule rejected the second of any swapped pair.
    std::vector<std::uint32_t> order;
    for (std::uint32_t packet = 0U; packet < 24U; ++packet) {
        order.push_back(packet);
    }
    std::swap(order[10], order[11]);

    std::vector<float> output(24U * packetFrames, 0.0F);
    std::uint64_t now = 0U;
    for (std::size_t step = 0U; step < order.size(); ++step) {
        const std::uint32_t sequence = order[step];
        receiver.submit(
            sequence,
            std::span<const float>(source.data() + sequence * packetFrames, packetFrames),
            now);
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(
            std::span<float>(output.data() + step * packetFrames, packetFrames)));
    }

    const auto telemetry = receiver.telemetry();
    EXPECT_TRUE(telemetry.packetsAccepted == 24U);
    EXPECT_TRUE(telemetry.packetsLate == 0U);
    EXPECT_TRUE(telemetry.packetsConcealed == 0U);
    // With no concealment, stretch, or trim, block i carries source packet
    // i - 1 exactly, which is what the comparison below relies on.
    EXPECT_TRUE(telemetry.bufferStretches == 0U);
    EXPECT_TRUE(telemetry.latencyTrims == 0U);

    // Both swapped packets are played in their correct positions. Block 1 is
    // the fade-in from priming silence, so comparison starts at block 2.
    for (std::size_t packet = 2U; packet < 24U; ++packet) {
        const auto produced = std::span<const float>(
            output.data() + packet * packetFrames, packetFrames);
        const auto expected = std::span<const float>(
            source.data() + (packet - 1U) * packetFrames, packetFrames);
        for (std::size_t index = 0U; index < packetFrames; ++index) {
            EXPECT_TRUE(produced[index] == expected[index]);
        }
    }
}

JAMLINK_TEST(receiver_rejects_duplicates_and_late_arrivals) {
    AudioStreamReceiver receiver(defaultSettings());
    const auto source = makeSource(packetFrames * 16U);
    std::vector<float> output(packetFrames, 0.0F);
    std::uint64_t now = 0U;

    for (std::uint32_t packet = 0U; packet < 8U; ++packet) {
        const auto block = std::span<const float>(
            source.data() + packet * packetFrames, packetFrames);
        receiver.submit(packet, block, now);
        receiver.submit(packet, block, now);
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }

    const auto telemetry = receiver.telemetry();
    // Every packet was offered twice; the second copy is a duplicate while it
    // is still queued, or late once playout has moved past it.
    EXPECT_TRUE(telemetry.packetsAccepted == 8U);
    EXPECT_TRUE(telemetry.packetsDuplicate + telemetry.packetsLate == 8U);
    EXPECT_TRUE(telemetry.packetsConcealed == 0U);

    // A packet from far in the past must be rejected, never played.
    const auto stale = std::span<const float>(source.data(), packetFrames);
    receiver.submit(0U, stale, now);
    EXPECT_TRUE(receiver.telemetry().packetsLate > telemetry.packetsLate);
}

JAMLINK_TEST(receive_depth_adapts_to_jitter_and_stays_bounded) {
    auto settings = defaultSettings();
    settings.shrinkHoldPackets = 40U;
    AudioStreamReceiver receiver(settings);
    const auto source = makeSource(packetFrames * 4U);
    const auto block = std::span<const float>(source.data(), packetFrames);
    std::vector<float> output(packetFrames, 0.0F);

    // Steady cadence first: the target must sit at the configured minimum.
    std::uint64_t now = 0U;
    for (std::uint32_t packet = 0U; packet < 200U; ++packet) {
        receiver.submit(packet, block, now);
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }
    const auto calm = receiver.telemetry();
    EXPECT_TRUE(calm.targetDepthFrames == 2U * packetFrames);

    // Now add 20 ms of arrival jitter, which is four packet periods.
    jamlink::testing::DeterministicChannel jitterSource(
        jamlink::testing::ImpairmentProfile{0U, 20'000U, 0U, 0U, 0U, 0U, 0U, 0U}, 7U);
    for (std::uint32_t packet = 200U; packet < 1'200U; ++packet) {
        jitterSource.offer(packet, now, packetPeriodMicroseconds);
        for (const auto& arrival : jitterSource.drainUntil(now)) {
            receiver.submit(arrival.sequence, block, arrival.arrivalMicroseconds);
        }
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }
    const auto disturbed = receiver.telemetry();
    EXPECT_TRUE(disturbed.targetDepthFrames > calm.targetDepthFrames);
    // Bounded growth: JamLink exists for interactive playing, so the buffer is
    // never allowed to hide instability behind unbounded latency.
    EXPECT_TRUE(disturbed.targetDepthFrames
        <= static_cast<std::uint32_t>(settings.maximumDepthPackets * packetFrames));
    std::cout << "    jitter adaptation: target depth "
              << (calm.targetDepthFrames * 1000U / sampleRate) << " ms -> "
              << (disturbed.targetDepthFrames * 1000U / sampleRate) << " ms\n";

    // Returning to a calm network must shrink the buffer back down.
    for (std::uint32_t packet = 1'200U; packet < 2'400U; ++packet) {
        receiver.submit(packet, block, now);
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }
    const auto settled = receiver.telemetry();
    EXPECT_TRUE(settled.targetDepthFrames < disturbed.targetDepthFrames);
    std::cout << "    jitter recovery: target depth returned to "
              << (settled.targetDepthFrames * 1000U / sampleRate) << " ms\n";
}

JAMLINK_TEST(receiver_holds_up_under_combined_impairment) {
    auto settings = defaultSettings();
    AudioStreamReceiver receiver(settings);
    const auto source = makeSource(packetFrames * 4U);
    const auto block = std::span<const float>(source.data(), packetFrames);
    std::vector<float> output(packetFrames, 0.0F);

    jamlink::testing::ImpairmentProfile profile;
    profile.baseLatencyMicroseconds = 18'000U;
    profile.jitterMicroseconds = 6'000U;
    profile.lossPerThousand = 10U;
    profile.burstStartPerThousand = 3U;
    profile.burstLengthPackets = 4U;
    profile.duplicatePerThousand = 5U;
    profile.reorderPerThousand = 20U;
    profile.reorderDepthPackets = 2U;
    jamlink::testing::DeterministicChannel channel(profile, 20'260'810U);

    constexpr std::uint32_t packetCount = 12'000U; // one virtual minute
    std::uint64_t now = 0U;
    for (std::uint32_t packet = 0U; packet < packetCount; ++packet) {
        channel.offer(packet, now, packetPeriodMicroseconds);
        now += packetPeriodMicroseconds;
        for (const auto& arrival : channel.drainUntil(now)) {
            receiver.submit(arrival.sequence, block, arrival.arrivalMicroseconds);
        }
        static_cast<void>(receiver.pull(output));
        EXPECT_TRUE(allFinite(output));
    }

    const auto telemetry = receiver.telemetry();
    const double lossRatio = static_cast<double>(channel.droppedPackets())
        / static_cast<double>(packetCount);
    const double concealRatio = static_cast<double>(telemetry.packetsConcealed)
        / static_cast<double>(packetCount);

    std::cout << "    combined impairment over " << packetCount << " packets:\n"
              << "      channel dropped " << channel.droppedPackets()
              << " (" << (lossRatio * 100.0) << "%), reordered "
              << channel.reorderedPackets() << ", duplicated "
              << channel.duplicatedPackets() << "\n"
              << "      receiver concealed " << telemetry.packetsConcealed
              << " (" << (concealRatio * 100.0) << "%), late "
              << telemetry.packetsLate << ", dropped " << telemetry.packetsDropped
              << ", stretches " << telemetry.bufferStretches
              << ", trims " << telemetry.latencyTrims << "\n"
              << "      steady-state depth "
              << (telemetry.currentDepthFrames * 1000U / sampleRate) << " ms, target "
              << (telemetry.targetDepthFrames * 1000U / sampleRate) << " ms\n";

    // Concealment must track real loss rather than masking a starved playout
    // loop. The 1.5x allowance leaves room for genuinely late packets.
    EXPECT_TRUE(concealRatio <= lossRatio * 1.5 + 0.002);
    // The buffer must actually hold audio; a depth that collapses to zero means
    // the adaptive target is not being realised.
    EXPECT_TRUE(telemetry.currentDepthFrames >= packetFrames);
    EXPECT_TRUE(telemetry.resyncs == 0U);
    // Reordered packets must be recovered, not thrown away.
    EXPECT_TRUE(telemetry.packetsReordered > 0U);
    EXPECT_TRUE(telemetry.currentDepthFrames
        <= static_cast<std::uint32_t>(settings.maximumDepthPackets * packetFrames));
}

JAMLINK_TEST(receiver_resynchronises_after_a_stream_restart) {
    AudioStreamReceiver receiver(defaultSettings());
    const auto source = makeSource(packetFrames * 4U);
    const auto block = std::span<const float>(source.data(), packetFrames);
    std::vector<float> output(packetFrames, 0.0F);

    std::uint64_t now = 0U;
    for (std::uint32_t packet = 0U; packet < 50U; ++packet) {
        receiver.submit(packet, block, now);
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }
    EXPECT_TRUE(receiver.telemetry().playing);

    // The peer restarts and its sequence numbering jumps far away.
    for (std::uint32_t packet = 900'000U; packet < 900'050U; ++packet) {
        receiver.submit(packet, block, now);
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }

    const auto telemetry = receiver.telemetry();
    EXPECT_TRUE(telemetry.resyncs >= 1U);
    EXPECT_TRUE(telemetry.playing);
    // After resynchronising, real audio must flow again rather than endless
    // concealment.
    EXPECT_TRUE(energy(output) > 0.0);
}

JAMLINK_TEST(receiver_handles_sequence_number_wraparound) {
    AudioStreamReceiver receiver(defaultSettings());
    const auto source = makeSource(packetFrames * 4U);
    const auto block = std::span<const float>(source.data(), packetFrames);
    std::vector<float> output(packetFrames, 0.0F);

    std::uint64_t now = 0U;
    const std::uint32_t start = 0xFFFFFFF0U;
    for (std::uint32_t step = 0U; step < 64U; ++step) {
        receiver.submit(start + step, block, now);
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }

    const auto telemetry = receiver.telemetry();
    // Wrapping past 2^32 is ordinary forward progress, not a restart.
    EXPECT_TRUE(telemetry.resyncs == 0U);
    EXPECT_TRUE(telemetry.packetsConcealed == 0U);
    EXPECT_TRUE(telemetry.packetsAccepted == 64U);
    EXPECT_TRUE(telemetry.playing);
}

JAMLINK_TEST(submit_and_pull_allocate_nothing_after_construction) {
    AudioStreamReceiver receiver(defaultSettings());
    const auto source = makeSource(packetFrames * 4U);
    const auto block = std::span<const float>(source.data(), packetFrames);
    std::vector<float> output(packetFrames, 0.0F);
    std::vector<float> shortOutput(64U, 0.0F);

    // Warm the receiver so priming, concealment, and recovery have all run at
    // least once before the trap is armed.
    std::uint64_t now = 0U;
    for (std::uint32_t packet = 0U; packet < 40U; ++packet) {
        if (packet != 12U && packet != 13U) {
            receiver.submit(packet, block, now);
        }
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
    }

    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_relaxed);
    for (std::uint32_t packet = 40U; packet < 400U; ++packet) {
        // Exercise loss, duplication, reorder, and partial reads together.
        if (packet % 17U != 0U) {
            receiver.submit(packet, block, now);
        }
        if (packet % 31U == 0U) {
            receiver.submit(packet, block, now);
        }
        if (packet % 23U == 0U && packet > 41U) {
            receiver.submit(packet - 1U, block, now);
        }
        now += packetPeriodMicroseconds;
        static_cast<void>(receiver.pull(output));
        static_cast<void>(receiver.pull(shortOutput));
        static_cast<void>(receiver.telemetry());
    }
    allocationTrackingEnabled.store(false, std::memory_order_relaxed);

    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_relaxed) == 0U);
}

JAMLINK_TEST(receiver_is_stable_across_a_long_virtual_session) {
    auto settings = defaultSettings();
    AudioStreamReceiver receiver(settings);
    const auto source = makeSource(packetFrames * 4U);
    const auto block = std::span<const float>(source.data(), packetFrames);
    std::vector<float> output(packetFrames, 0.0F);

    jamlink::testing::ImpairmentProfile profile;
    profile.baseLatencyMicroseconds = 25'000U;
    profile.jitterMicroseconds = 8'000U;
    profile.lossPerThousand = 5U;
    profile.burstStartPerThousand = 1U;
    profile.burstLengthPackets = 6U;
    profile.reorderPerThousand = 10U;
    jamlink::testing::DeterministicChannel channel(profile, 4242U);

    // 720 000 packets is one virtual hour at 5 ms packetisation.
    constexpr std::uint32_t packetCount = 720'000U;
    std::uint64_t now = 0U;
    std::uint32_t peakDepthFrames = 0U;
    for (std::uint32_t packet = 0U; packet < packetCount; ++packet) {
        channel.offer(packet, now, packetPeriodMicroseconds);
        now += packetPeriodMicroseconds;
        for (const auto& arrival : channel.drainUntil(now)) {
            receiver.submit(arrival.sequence, block, arrival.arrivalMicroseconds);
        }
        static_cast<void>(receiver.pull(output));
        peakDepthFrames = std::max(peakDepthFrames, receiver.telemetry().currentDepthFrames);
    }

    const auto telemetry = receiver.telemetry();
    EXPECT_TRUE(allFinite(output));
    EXPECT_TRUE(telemetry.playing);
    EXPECT_TRUE(peakDepthFrames
        <= static_cast<std::uint32_t>(settings.maximumDepthPackets * packetFrames));
    const double lossRatio = static_cast<double>(channel.droppedPackets())
        / static_cast<double>(packetCount);
    const double concealRatio = static_cast<double>(telemetry.packetsConcealed)
        / static_cast<double>(packetCount);
    std::cout << "    one virtual hour: peak depth "
              << (peakDepthFrames * 1000U / sampleRate) << " ms, channel loss "
              << (lossRatio * 100.0) << "%, concealed "
              << telemetry.packetsConcealed << " (" << (concealRatio * 100.0)
              << "%), stretches " << telemetry.bufferStretches
              << ", trims " << telemetry.latencyTrims
              << ", resyncs " << telemetry.resyncs << "\n";
    EXPECT_TRUE(telemetry.resyncs == 0U);
    // Over an hour the concealment rate must stay pinned to real loss.
    EXPECT_TRUE(concealRatio <= lossRatio * 1.5 + 0.002);
    EXPECT_TRUE(telemetry.currentDepthFrames >= packetFrames);
}

JAMLINK_TEST(receiver_rejects_inconsistent_configuration) {
    const auto expectRejected = [](const AudioStreamReceiverSettings& settings) {
        try {
            AudioStreamReceiver receiver(settings);
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };

    auto notPowerOfTwo = defaultSettings();
    notPowerOfTwo.slotCount = 100U;
    EXPECT_TRUE(expectRejected(notPowerOfTwo));

    auto tooSmall = defaultSettings();
    tooSmall.slotCount = 32U;
    tooSmall.maximumDepthPackets = 32U;
    EXPECT_TRUE(expectRejected(tooSmall));

    auto inverted = defaultSettings();
    inverted.minimumDepthPackets = 8U;
    inverted.maximumDepthPackets = 4U;
    EXPECT_TRUE(expectRejected(inverted));

    auto badRate = defaultSettings();
    badRate.sampleRate = 100U;
    EXPECT_TRUE(expectRejected(badRate));
}

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& test : tests()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests().size() - failures) << '/' << tests().size()
              << " network tests passed\n";
    return failures == 0U ? 0 : 1;
}
