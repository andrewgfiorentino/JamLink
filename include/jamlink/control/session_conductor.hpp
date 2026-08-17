// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace jamlink::control {

// One answer to the only question a musician is really asking.
//
// Every subsystem already knows something true: the audio service knows whether
// a device opened, the transport knows whether a socket is bound and a peer
// authenticated, the receive path knows whether packets are arriving. None of
// them knows whether two people can play, and asking the interface to work that
// out from a dozen unrelated states is how a room ends up reporting "connected"
// while nobody can hear anything.
//
// This is that missing layer. It is deliberately a pure function of evidence
// with no clock, no threads and no I/O of its own, so every transition below is
// reproducible in a test rather than only observable in a live session.
enum class SessionPhase : std::uint8_t {
    Idle,
    CheckingAudio,
    CheckingNetwork,
    ReadyToInvite,
    WaitingForPeer,
    Joining,
    Connecting,
    ReadyToPlay,
    Degraded,
    Reconnecting,
    FinalizingRecording,
    Completed,
    FailedRecoverable,
    FailedFinal,
};

[[nodiscard]] std::string_view phaseName(SessionPhase phase) noexcept;

// The one thing worth doing next. Never a list.
enum class GuidanceAction : std::uint8_t {
    None,
    FinishSoundCheck,
    ReconnectAudioDevice,
    LowerInputGain,
    ChooseLargerBuffer,
    FixFirewall,
    AskFriendToHost,
    SendInvite,
    WaitForFriend,
    Retry,
    ReviewRecording,
    LeaveRoom,
};

// What the room is doing about a problem, so guidance can say whether the
// musician needs to act or simply wait.
enum class RecoveryPosture : std::uint8_t {
    Nothing,
    Compensating,
    Reconnecting,
    NeedsMusician,
    GaveUp,
};

// Facts gathered from the subsystems. Every field is something a subsystem
// already knows; none of it is inferred here.
struct SessionEvidence final {
    // A session is wanted at all. Without this the conductor stays idle no
    // matter how healthy everything else looks.
    bool sessionRequested{false};
    bool joiningRatherThanHosting{false};
    bool leaveRequested{false};

    // Audio.
    bool audioDevicesPresent{false};
    bool audioRunning{false};
    bool soundCheckVerified{false};
    // A device that was working and vanished, which is recoverable and must not
    // be confused with never having had one.
    bool audioDeviceLost{false};
    bool inputClipping{false};
    // Blocks the device failed to hand over in the recent past.
    std::uint64_t recentAudioDropouts{0U};

    // Network preparation.
    bool udpBound{false};
    bool firewallBlocking{false};
    bool portMappingRefused{false};
    bool publicAddressKnown{false};
    // This machine cannot be reached, but can still reach out.
    bool canJoinButNotHost{false};

    // Transport.
    bool peerAuthenticated{false};
    bool buildCompatible{true};
    bool transportFailed{false};
    // The peer was here and is gone, as opposed to never having arrived.
    bool peerWasConnected{false};

    // Media. The evidence that separates "a socket exists" from "we can play".
    bool mediaProgressing{false};
    std::uint32_t concealedPerThousand{0U};
    std::uint32_t receiveBufferMilliseconds{0U};
    bool roundTripMeasured{false};
    std::uint32_t roundTripMilliseconds{0U};

    // Recording.
    bool recording{false};
    bool recordingFinalizing{false};
    bool recordingNeedsReview{false};
    bool recordingFailed{false};
    bool storageFallingBehind{false};
};

// What the interface shows. One headline, one explanation, one action.
struct MusicianGuidance final {
    SessionPhase phase{SessionPhase::Idle};
    std::string_view headline;
    std::string_view explanation;
    GuidanceAction action{GuidanceAction::None};
    std::string_view actionLabel;
    bool actionEnabled{false};
    RecoveryPosture posture{RecoveryPosture::Nothing};
    // True while the musician can play, which is the only thing some of the
    // interface needs to know.
    bool playable{false};
};

// A semantic record of how the session got here, for diagnostics and the
// support bundle. Deliberately holds no secrets, no endpoints and no chat.
struct PhaseTransition final {
    SessionPhase from{SessionPhase::Idle};
    SessionPhase to{SessionPhase::Idle};
    std::uint64_t atMillisecond{0U};
};

class SessionConductor final {
public:
    // Bounded so a long session cannot grow without limit; the oldest
    // transitions are dropped rather than the newest, because what a session is
    // doing now matters more for support than how it started.
    static constexpr std::size_t historyCapacity = 64U;

    SessionConductor() = default;

    // Idempotent: the same evidence twice records one transition, not two.
    MusicianGuidance update(const SessionEvidence& evidence, std::uint64_t atMillisecond) noexcept;

    [[nodiscard]] SessionPhase phase() const noexcept { return phase_; }
    [[nodiscard]] MusicianGuidance guidance() const noexcept { return guidance_; }

    [[nodiscard]] std::size_t transitionCount() const noexcept { return historyCount_; }
    // Oldest first, up to historyCapacity.
    [[nodiscard]] PhaseTransition transitionAt(std::size_t index) const noexcept;

    void reset() noexcept;

private:
    [[nodiscard]] SessionPhase evaluate(const SessionEvidence& evidence) const noexcept;
    void record(SessionPhase next, std::uint64_t atMillisecond) noexcept;

    SessionPhase phase_{SessionPhase::Idle};
    MusicianGuidance guidance_{};
    std::array<PhaseTransition, historyCapacity> history_{};
    std::size_t historyStart_{0U};
    std::size_t historyCount_{0U};
};

// Exposed for tests: the guidance any phase produces from given evidence.
[[nodiscard]] MusicianGuidance guidanceFor(
    SessionPhase phase, const SessionEvidence& evidence) noexcept;

} // namespace jamlink::control
