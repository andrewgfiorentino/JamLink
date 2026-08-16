// SPDX-License-Identifier: GPL-3.0-or-later

// What the outgoing send path owes the person on the other end.
//
// Every one of these tests exists because a live session went wrong in a way
// that a screenshot could not distinguish from a bad network. A tester's
// guitar arrived bit-crushed and glitching while the link itself carried a 4 ms
// round trip; the send rate sat near 120 packets per second per stream where
// 200 was owed, and the shortfall was read as loss rather than as audio the
// sender never transmitted. These assertions make that shortfall a build
// failure instead of a diagnosis.

#include "jamlink/network/outgoing_audio_pacer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

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

using jamlink::network::OutgoingAudioPacer;

constexpr std::size_t packetFrames = 240U;
constexpr std::uint32_t networkSampleRate = 48'000U;
constexpr std::uint64_t packetIntervalMicroseconds = 5'000U;
// Two hundred milliseconds of capture. Beyond this a live session cannot use
// the audio, so holding it would only delay everything behind it.
constexpr std::size_t maximumBacklogFrames = 9'600U;
// One packet of audio every five milliseconds, which is the rate every one of
// these tests is ultimately checking.
constexpr std::uint64_t packetsPerSecond = 200U;

[[nodiscard]] OutgoingAudioPacer makePacer(std::uint32_t sourceRate) {
    OutgoingAudioPacer pacer(packetFrames, networkSampleRate, maximumBacklogFrames);
    if (!pacer.setSourceRate(sourceRate)) {
        throw std::runtime_error("source rate rejected");
    }
    return pacer;
}

// Drives a pacer through virtual time: audio is produced at exactly the source
// device's rate and the sender wakes on a fixed period, which is the situation
// every real session is in.
struct RunResult final {
    std::uint64_t packetsReleased{0U};
    std::size_t largestReleaseBurst{0U};
    std::size_t finalBacklogFrames{0U};
    std::size_t largestBacklogFrames{0U};
    std::uint64_t framesDiscarded{0U};
    std::uint64_t framesAccepted{0U};
    float smallestReleasedMagnitude{1.0F};
};

[[nodiscard]] RunResult run(
    OutgoingAudioPacer& pacer,
    std::uint32_t sourceRate,
    std::uint64_t durationMicroseconds,
    std::uint64_t wakeIntervalMicroseconds,
    float level) {
    RunResult result;
    std::vector<float> packet(packetFrames, 0.0F);
    std::vector<float> captured;
    // Fractional frames carry across wake-ups so the producer stays on the
    // device's rate rather than being rounded up every period.
    double owedFrames = 0.0;
    for (std::uint64_t now = 0U; now < durationMicroseconds;
         now += wakeIntervalMicroseconds) {
        owedFrames += static_cast<double>(wakeIntervalMicroseconds)
            * static_cast<double>(sourceRate) / 1'000'000.0;
        const auto produce = static_cast<std::size_t>(owedFrames);
        owedFrames -= static_cast<double>(produce);
        captured.assign(produce, level);
        pacer.accept(std::span<const float>(captured.data(), captured.size()));

        std::size_t burst = 0U;
        while (pacer.release(now, std::span<float>(packet.data(), packet.size()))) {
            ++burst;
            for (const float sample : packet) {
                result.smallestReleasedMagnitude =
                    std::min(result.smallestReleasedMagnitude, std::abs(sample));
            }
        }
        result.largestReleaseBurst = std::max(result.largestReleaseBurst, burst);
        const auto telemetry = pacer.telemetry();
        result.largestBacklogFrames =
            std::max(result.largestBacklogFrames, telemetry.backlogFrames);
    }
    const auto telemetry = pacer.telemetry();
    result.packetsReleased = telemetry.packetsReleased;
    result.finalBacklogFrames = telemetry.backlogFrames;
    result.framesDiscarded = telemetry.framesDiscarded;
    result.framesAccepted = telemetry.framesAccepted;
    return result;
}

// A stream that keeps up owes one packet every five milliseconds, and owes an
// account of every frame it was given.
void expectMediaRate(
    const RunResult& result,
    std::uint64_t seconds,
    std::uint32_t sourceRate) {
    const std::uint64_t ideal = packetsPerSecond * seconds;
    // Never faster than the media clock. Exceeding it is the burst behaviour
    // that made a 4 ms link measure as a jittery one.
    EXPECT_TRUE(result.packetsReleased <= ideal);

    // Conservation. Every captured frame was either sent, is still queued, or
    // was counted as discarded. This is the assertion that makes stranding
    // impossible to ship: audio cannot quietly go missing between the capture
    // callback and the socket.
    const double sourceFramesPerPacket = static_cast<double>(packetFrames)
        * static_cast<double>(sourceRate) / static_cast<double>(networkSampleRate);
    const double accountedFor =
        static_cast<double>(result.packetsReleased) * sourceFramesPerPacket
        + static_cast<double>(result.framesDiscarded)
        + static_cast<double>(result.finalBacklogFrames);
    const auto accepted = static_cast<double>(result.framesAccepted);
    // The converter's drift correction moves the step by up to 500 ppm, so the
    // per-packet consumption is not exactly nominal.
    EXPECT_TRUE(std::abs(accountedFor - accepted) < accepted * 0.005 + 512.0);

    // The only audio a healthy stream is allowed to discard is the converter's
    // one-time startup priming, which is well under half a second.
    EXPECT_TRUE(result.framesDiscarded < sourceRate / 2U);

    // And once that startup cost is accounted for, the rate owed is met.
    const auto startupCost = static_cast<std::uint64_t>(
        static_cast<double>(result.framesDiscarded) / sourceFramesPerPacket);
    EXPECT_TRUE(result.packetsReleased + startupCost + 4U >= ideal);
}

JAMLINK_TEST(steadyCaptureSustainsTheMediaRate) {
    auto pacer = makePacer(48'000U);
    // Ten seconds of capture arriving in ten-millisecond blocks.
    const RunResult result = run(pacer, 48'000U, 10'000'000U, 10'000U, 0.5F);
    expectMediaRate(result, 10U, 48'000U);
}

JAMLINK_TEST(captureRateDifferentFromNetworkRateSustainsTheMediaRate) {
    // An interface running at 44.1 kHz still owes 48 kHz packets at the same
    // cadence; the converter absorbs the rate difference, not the schedule.
    auto pacer = makePacer(44'100U);
    const RunResult result = run(pacer, 44'100U, 10'000'000U, 10'000U, 0.5F);
    expectMediaRate(result, 10U, 44'100U);
}

JAMLINK_TEST(coarseWakeUpsDoNotStrandCapturedAudio) {
    // The regression. Windows will not honour a five-millisecond select timeout
    // reliably; the sender commonly wakes on the system tick near 15.6 ms. The
    // design this replaces capped catch-up at four packets and then rebased the
    // schedule forward, abandoning everything behind it, so the backlog climbed
    // every wake-up until the converter overran and discarded in chunks. Thirty
    // seconds is long enough for that drift to be unmistakable.
    auto pacer = makePacer(48'000U);
    const RunResult result = run(pacer, 48'000U, 30'000'000U, 15'625U, 0.5F);
    expectMediaRate(result, 30U, 48'000U);
    // Backlog must stay near the converter's working fill rather than growing
    // with elapsed time.
    EXPECT_TRUE(result.largestBacklogFrames < 2'000U);
}

JAMLINK_TEST(veryCoarseWakeUpsStillSustainTheMediaRate) {
    // A heavily loaded machine can wake far less often than the tick. The rate
    // owed does not change, and neither does the ban on stranding audio.
    auto pacer = makePacer(48'000U);
    const RunResult result = run(pacer, 48'000U, 20'000'000U, 50'000U, 0.5F);
    expectMediaRate(result, 20U, 48'000U);
}

JAMLINK_TEST(packetsDoNotClumpWhenTheSenderKeepsUp) {
    // A receiver cannot tell a sender's bursts from network jitter, so a sender
    // that wakes on the media clock must release one packet per wake, not a
    // backlog.
    auto pacer = makePacer(48'000U);
    const RunResult result =
        run(pacer, 48'000U, 5'000'000U, packetIntervalMicroseconds, 0.5F);
    expectMediaRate(result, 5U, 48'000U);
    EXPECT_TRUE(result.largestReleaseBurst <= 2U);
}

JAMLINK_TEST(releasedAudioIsCapturedAudioAndNeverManufacturedSilence) {
    // The defect this guards. Outgoing audio used to be read from a converter
    // whose return value was discarded; on an underrun that converter zero-fills
    // its destination, so a block of digital silence was transmitted as though
    // it were the captured guitar. A constant input must leave as a constant.
    auto pacer = makePacer(48'000U);
    const RunResult result = run(pacer, 48'000U, 5'000'000U, 10'000U, 0.5F);
    expectMediaRate(result, 5U, 48'000U);
    EXPECT_TRUE(result.smallestReleasedMagnitude > 0.49F);
}

JAMLINK_TEST(aStalledSenderDiscardsOnlyWhatItCountsAndThenRecovers) {
    auto pacer = makePacer(48'000U);
    std::vector<float> packet(packetFrames, 0.0F);
    std::vector<float> captured(480U, 0.5F);

    // Half a second where audio is captured but the sender never runs, which is
    // what a blocked socket thread looks like.
    std::uint64_t now = 0U;
    for (; now < 500'000U; now += 10'000U) {
        pacer.accept(std::span<const float>(captured.data(), captured.size()));
    }
    const auto stalled = pacer.telemetry();
    // Backlog is bounded and whatever was dropped was counted, rather than the
    // converter silently overrunning.
    EXPECT_TRUE(stalled.backlogFrames <= maximumBacklogFrames);
    EXPECT_TRUE(stalled.framesDiscarded > 0U);

    // Five seconds of normal operation afterwards must return to the media rate.
    const std::uint64_t releasedBeforeRecovery = stalled.packetsReleased;
    for (const std::uint64_t deadline = now + 5'000'000U; now < deadline; now += 10'000U) {
        pacer.accept(std::span<const float>(captured.data(), captured.size()));
        while (pacer.release(now, std::span<float>(packet.data(), packet.size()))) {
        }
    }
    const auto recovered = pacer.telemetry();
    const std::uint64_t releasedDuringRecovery =
        recovered.packetsReleased - releasedBeforeRecovery;
    // The backlog is drained on top of the five seconds owed, so the count is
    // allowed to exceed the media rate here; what matters is that it is not
    // short and that the backlog came back down.
    EXPECT_TRUE(releasedDuringRecovery + 4U >= packetsPerSecond * 5U);
    EXPECT_TRUE(recovered.backlogFrames < 2'000U);
}

JAMLINK_TEST(aStoppedCaptureDoesNotBankADeficitAndThenBurst) {
    // A stream that stops and restarts must not release every deadline it
    // missed the moment audio returns.
    auto pacer = makePacer(48'000U);
    std::vector<float> packet(packetFrames, 0.0F);
    std::vector<float> captured(480U, 0.5F);

    std::uint64_t now = 0U;
    for (const std::uint64_t deadline = now + 1'000'000U; now < deadline; now += 10'000U) {
        pacer.accept(std::span<const float>(captured.data(), captured.size()));
        while (pacer.release(now, std::span<float>(packet.data(), packet.size()))) {
        }
    }
    // Two seconds with the instrument silent: nothing is captured at all.
    for (const std::uint64_t deadline = now + 2'000'000U; now < deadline; now += 10'000U) {
        while (pacer.release(now, std::span<float>(packet.data(), packet.size()))) {
        }
    }
    // Capture resumes.
    std::size_t largestBurst = 0U;
    for (const std::uint64_t deadline = now + 1'000'000U; now < deadline; now += 10'000U) {
        pacer.accept(std::span<const float>(captured.data(), captured.size()));
        std::size_t burst = 0U;
        while (pacer.release(now, std::span<float>(packet.data(), packet.size()))) {
            ++burst;
        }
        largestBurst = std::max(largestBurst, burst);
    }
    // A ten-millisecond wake-up owes two packets. Anything approaching the four
    // hundred deadlines that passed in silence would be the burst.
    EXPECT_TRUE(largestBurst <= 4U);
}

JAMLINK_TEST(anUnsupportedSourceRateIsRefusedWithoutStoppingTheStream) {
    OutgoingAudioPacer pacer(packetFrames, networkSampleRate, maximumBacklogFrames);
    EXPECT_TRUE(pacer.setSourceRate(48'000U));
    EXPECT_TRUE(!pacer.setSourceRate(2'000U));
    // The working rate survives the rejected one.
    std::vector<float> captured(4'800U, 0.5F);
    std::vector<float> packet(packetFrames, 0.0F);
    pacer.accept(std::span<const float>(captured.data(), captured.size()));
    EXPECT_TRUE(pacer.release(1'000'000U, std::span<float>(packet.data(), packet.size())));
    EXPECT_TRUE(std::abs(packet.front() - 0.5F) < 0.01F);
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
              << " outgoing audio pacer tests passed\n";
    return failures == 0U ? 0 : 1;
}
