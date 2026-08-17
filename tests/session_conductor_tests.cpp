// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// What the session conductor owes a musician.
//
// The rule these all serve: a phase that says someone can play must be gated on
// evidence that audio is moving, never on a socket existing. Everything else
// here is a consequence of that, or of telling one person one useful thing.

#include "jamlink/control/session_conductor.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
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

using jamlink::control::GuidanceAction;
using jamlink::control::RecoveryPosture;
using jamlink::control::SessionConductor;
using jamlink::control::SessionEvidence;
using jamlink::control::SessionPhase;

// Everything true that a healthy two-person session needs. Individual tests
// take this away one fact at a time, which is how each gate is proved to be
// load-bearing rather than decorative.
[[nodiscard]] SessionEvidence healthy() {
    SessionEvidence evidence;
    evidence.sessionRequested = true;
    evidence.audioDevicesPresent = true;
    evidence.audioRunning = true;
    evidence.soundCheckVerified = true;
    evidence.udpBound = true;
    evidence.publicAddressKnown = true;
    evidence.peerAuthenticated = true;
    evidence.buildCompatible = true;
    evidence.mediaProgressing = true;
    evidence.roundTripMeasured = true;
    evidence.roundTripMilliseconds = 12U;
    evidence.receiveBufferMilliseconds = 15U;
    return evidence;
}

JAMLINK_TEST(a_healthy_session_is_ready_to_play) {
    SessionConductor conductor;
    const auto guidance = conductor.update(healthy(), 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::ReadyToPlay);
    EXPECT_TRUE(guidance.playable);
    EXPECT_TRUE(guidance.headline == "Ready to play");
    EXPECT_TRUE(guidance.action == GuidanceAction::None);
}

JAMLINK_TEST(an_authenticated_peer_is_not_by_itself_ready_to_play) {
    // The gate the whole design exists for. A socket and a handshake prove two
    // programs agree with each other, not that two people can hear each other.
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.mediaProgressing = false;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::Connecting);
    EXPECT_TRUE(!guidance.playable);
}

JAMLINK_TEST(audio_is_settled_before_the_network_is_discussed) {
    // Being reachable is worth nothing without something to play into, so the
    // musician is never sent to fix a router while their interface is missing.
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.audioDevicesPresent = false;
    evidence.firewallBlocking = true;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::CheckingAudio);
    EXPECT_TRUE(guidance.action == GuidanceAction::ReconnectAudioDevice);
}

JAMLINK_TEST(a_struggling_connection_still_lets_them_play) {
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.concealedPerThousand = 80U;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::Degraded);
    // The distinction that matters: degraded is not broken.
    EXPECT_TRUE(guidance.playable);
    EXPECT_TRUE(guidance.posture == RecoveryPosture::Compensating);
    EXPECT_TRUE(guidance.action == GuidanceAction::None);
}

JAMLINK_TEST(a_peer_that_was_here_and_is_gone_is_reconnecting_not_failed) {
    // A dropped connection must not throw the room away; the musician sees the
    // room being restored rather than being told to start again.
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.peerAuthenticated = false;
    evidence.peerWasConnected = true;
    const auto guidance = conductor.update(evidence, 2'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::Reconnecting);
    EXPECT_TRUE(guidance.posture == RecoveryPosture::Reconnecting);
}

JAMLINK_TEST(a_router_that_will_not_map_recommends_swapping_roles) {
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.peerAuthenticated = false;
    evidence.publicAddressKnown = false;
    evidence.canJoinButNotHost = true;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::CheckingNetwork);
    EXPECT_TRUE(guidance.action == GuidanceAction::AskFriendToHost);
    EXPECT_TRUE(guidance.actionEnabled);
}

JAMLINK_TEST(a_build_mismatch_is_the_one_failure_that_is_final) {
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.buildCompatible = false;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::FailedFinal);
    EXPECT_TRUE(guidance.posture == RecoveryPosture::GaveUp);
}

JAMLINK_TEST(a_take_being_written_outranks_everything_else) {
    // Losing a recording to a tidy shutdown is worse than any delay, so
    // finalising wins even over a connection that has fallen over.
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.leaveRequested = true;
    evidence.transportFailed = true;
    evidence.recordingFinalizing = true;
    const auto guidance = conductor.update(evidence, 3'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::FinalizingRecording);
}

JAMLINK_TEST(an_interrupted_take_asks_to_be_reviewed_rather_than_claiming_success) {
    SessionConductor conductor;
    SessionEvidence evidence;
    evidence.recordingNeedsReview = true;
    const auto guidance = conductor.update(evidence, 4'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::FailedRecoverable);
    EXPECT_TRUE(guidance.action == GuidanceAction::ReviewRecording);
    EXPECT_TRUE(guidance.headline == "Recovered recording");
}

JAMLINK_TEST(only_one_thing_is_asked_of_the_musician_at_a_time) {
    // Clipping matters, but not while they cannot play at all. It speaks only
    // once nothing more important is wrong.
    SessionConductor conductor;
    auto blocked = healthy();
    blocked.mediaProgressing = false;
    blocked.inputClipping = true;
    EXPECT_TRUE(conductor.update(blocked, 1'000U).action == GuidanceAction::None);

    auto playing = healthy();
    playing.inputClipping = true;
    const auto guidance = conductor.update(playing, 2'000U);
    EXPECT_TRUE(guidance.action == GuidanceAction::LowerInputGain);
    EXPECT_TRUE(guidance.playable);
}

JAMLINK_TEST(a_local_dropout_is_not_blamed_on_the_connection) {
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.recentAudioDropouts = 4U;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.action == GuidanceAction::ChooseLargerBuffer);
    EXPECT_TRUE(guidance.explanation.find("not") != std::string_view::npos);
}

JAMLINK_TEST(the_same_evidence_twice_records_one_transition) {
    SessionConductor conductor;
    const auto evidence = healthy();
    static_cast<void>(conductor.update(evidence, 1'000U));
    static_cast<void>(conductor.update(evidence, 1'100U));
    static_cast<void>(conductor.update(evidence, 1'200U));
    EXPECT_TRUE(conductor.phase() == SessionPhase::ReadyToPlay);
    // Idle -> ReadyToPlay, and nothing after it.
    EXPECT_TRUE(conductor.transitionCount() == 1U);
}

JAMLINK_TEST(the_lifecycle_is_recorded_in_order_for_diagnostics) {
    SessionConductor conductor;
    auto evidence = healthy();
    static_cast<void>(conductor.update(evidence, 1'000U));

    evidence.concealedPerThousand = 90U;
    static_cast<void>(conductor.update(evidence, 2'000U));

    evidence.concealedPerThousand = 0U;
    evidence.peerAuthenticated = false;
    evidence.peerWasConnected = true;
    static_cast<void>(conductor.update(evidence, 3'000U));

    evidence.peerAuthenticated = true;
    static_cast<void>(conductor.update(evidence, 4'000U));

    EXPECT_TRUE(conductor.transitionCount() == 4U);
    EXPECT_TRUE(conductor.transitionAt(0U).to == SessionPhase::ReadyToPlay);
    EXPECT_TRUE(conductor.transitionAt(1U).to == SessionPhase::Degraded);
    EXPECT_TRUE(conductor.transitionAt(2U).to == SessionPhase::Reconnecting);
    EXPECT_TRUE(conductor.transitionAt(3U).to == SessionPhase::ReadyToPlay);
    EXPECT_TRUE(conductor.transitionAt(3U).from == SessionPhase::Reconnecting);
    EXPECT_TRUE(conductor.transitionAt(3U).atMillisecond == 4'000U);
}

JAMLINK_TEST(the_history_is_bounded_and_keeps_the_most_recent) {
    // A long session must not grow without limit, and what it is doing now
    // matters more for support than how it started.
    SessionConductor conductor;
    auto evidence = healthy();
    for (std::uint64_t step = 0U; step < SessionConductor::historyCapacity * 3U; ++step) {
        evidence.concealedPerThousand = (step % 2U == 0U) ? 0U : 90U;
        static_cast<void>(conductor.update(evidence, step * 100U));
    }
    EXPECT_TRUE(conductor.transitionCount() == SessionConductor::historyCapacity);
    const auto newest = conductor.transitionAt(SessionConductor::historyCapacity - 1U);
    const auto oldest = conductor.transitionAt(0U);
    EXPECT_TRUE(newest.atMillisecond > oldest.atMillisecond);
}

JAMLINK_TEST(a_lost_device_keeps_the_session_recoverable) {
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.audioDeviceLost = true;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::FailedRecoverable);
    EXPECT_TRUE(guidance.action == GuidanceAction::ReconnectAudioDevice);
    EXPECT_TRUE(guidance.headline == "Audio interface disconnected");

    // And it comes back without the musician starting over.
    evidence.audioDeviceLost = false;
    EXPECT_TRUE(conductor.update(evidence, 2'000U).phase == SessionPhase::ReadyToPlay);
}

JAMLINK_TEST(no_session_requested_stays_idle_however_healthy_everything_is) {
    SessionConductor conductor;
    auto evidence = healthy();
    evidence.sessionRequested = false;
    const auto guidance = conductor.update(evidence, 1'000U);
    EXPECT_TRUE(guidance.phase == SessionPhase::Idle);
    EXPECT_TRUE(!guidance.playable);
}

JAMLINK_TEST(every_phase_offers_a_headline_and_an_actionable_label_when_it_asks_for_one) {
    // No phase may reach the interface with nothing to say, and no phase may
    // offer an enabled button without a label on it.
    SessionConductor conductor;
    const SessionEvidence evidence = healthy();
    for (std::uint8_t raw = 0U; raw <= static_cast<std::uint8_t>(SessionPhase::FailedFinal);
         ++raw) {
        const auto phase = static_cast<SessionPhase>(raw);
        const auto guidance = jamlink::control::guidanceFor(phase, evidence);
        EXPECT_TRUE(!guidance.headline.empty());
        EXPECT_TRUE(!guidance.explanation.empty());
        if (guidance.actionEnabled) {
            EXPECT_TRUE(guidance.action != GuidanceAction::None);
            EXPECT_TRUE(!guidance.actionLabel.empty());
        }
        EXPECT_TRUE(!jamlink::control::phaseName(phase).empty());
    }
    static_cast<void>(conductor);
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
              << " session conductor tests passed\n";
    return failures == 0U ? 0 : 1;
}
