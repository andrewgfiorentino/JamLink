// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/control/session_conductor.hpp"

namespace jamlink::control {
namespace {

// Past this much concealment the audio is audibly damaged rather than merely
// imperfect, and saying so beats letting a musician wonder whether it is them.
constexpr std::uint32_t strugglingConcealmentPerThousand = 30U;
// Buffering this deep means playing together has stopped being realistic.
constexpr std::uint32_t strugglingBufferMilliseconds = 60U;

[[nodiscard]] bool struggling(const SessionEvidence& evidence) noexcept {
    return evidence.concealedPerThousand >= strugglingConcealmentPerThousand
        || evidence.receiveBufferMilliseconds >= strugglingBufferMilliseconds;
}

} // namespace

std::string_view phaseName(SessionPhase phase) noexcept {
    switch (phase) {
    case SessionPhase::Idle: return "Idle";
    case SessionPhase::CheckingAudio: return "CheckingAudio";
    case SessionPhase::CheckingNetwork: return "CheckingNetwork";
    case SessionPhase::ReadyToInvite: return "ReadyToInvite";
    case SessionPhase::WaitingForPeer: return "WaitingForPeer";
    case SessionPhase::Joining: return "Joining";
    case SessionPhase::Connecting: return "Connecting";
    case SessionPhase::ReadyToPlay: return "ReadyToPlay";
    case SessionPhase::Degraded: return "Degraded";
    case SessionPhase::Reconnecting: return "Reconnecting";
    case SessionPhase::FinalizingRecording: return "FinalizingRecording";
    case SessionPhase::Completed: return "Completed";
    case SessionPhase::FailedRecoverable: return "FailedRecoverable";
    case SessionPhase::FailedFinal: return "FailedFinal";
    }
    return "Unknown";
}

// The order of these checks is the design.
//
// Anything that makes playing impossible is decided before anything that merely
// makes it worse, and every phase that claims a musician can play is gated on
// evidence that audio is actually moving rather than on a socket existing.
SessionPhase SessionConductor::evaluate(const SessionEvidence& evidence) const noexcept {
    // Finishing takes precedence over everything: a take being written is the
    // one thing worth waiting for after the music stops.
    if (evidence.recordingFinalizing) {
        return SessionPhase::FinalizingRecording;
    }
    if (evidence.recordingNeedsReview) {
        return SessionPhase::FailedRecoverable;
    }
    if (evidence.leaveRequested) {
        return SessionPhase::Completed;
    }
    if (!evidence.sessionRequested) {
        return SessionPhase::Idle;
    }

    // A build mismatch cannot be recovered from by waiting or retrying, so it
    // is the one failure that is final.
    if (!evidence.buildCompatible) {
        return SessionPhase::FailedFinal;
    }

    // Audio first. Being reachable is worthless without something to play into.
    if (evidence.audioDeviceLost) {
        return SessionPhase::FailedRecoverable;
    }
    if (!evidence.audioDevicesPresent || !evidence.audioRunning
        || !evidence.soundCheckVerified) {
        return SessionPhase::CheckingAudio;
    }

    // Then the parts of the network that decide whether an invite can work.
    if (!evidence.udpBound) {
        return SessionPhase::CheckingNetwork;
    }

    if (evidence.peerAuthenticated) {
        // A peer that has authenticated is not yet a peer you can play with.
        // Media has to be moving, which is the distinction that matters and the
        // one a socket cannot make.
        if (!evidence.mediaProgressing) {
            return evidence.peerWasConnected
                ? SessionPhase::Reconnecting : SessionPhase::Connecting;
        }
        return struggling(evidence) ? SessionPhase::Degraded : SessionPhase::ReadyToPlay;
    }

    // The peer was here and is not now. Transport failure during a live session
    // is recoverable; the room is not thrown away for it.
    if (evidence.peerWasConnected) {
        return evidence.transportFailed
            ? SessionPhase::FailedRecoverable : SessionPhase::Reconnecting;
    }
    if (evidence.transportFailed) {
        return SessionPhase::FailedRecoverable;
    }

    if (evidence.joiningRatherThanHosting) {
        return SessionPhase::Joining;
    }
    // Hosting needs to be reachable. Where it is not, the useful instruction is
    // to swap roles rather than to explain port forwarding.
    if (evidence.firewallBlocking || evidence.canJoinButNotHost) {
        return SessionPhase::CheckingNetwork;
    }
    return evidence.publicAddressKnown || evidence.portMappingRefused
        ? SessionPhase::WaitingForPeer : SessionPhase::ReadyToInvite;
}

MusicianGuidance guidanceFor(
    SessionPhase phase, const SessionEvidence& evidence) noexcept {
    MusicianGuidance guidance;
    guidance.phase = phase;

    switch (phase) {
    case SessionPhase::Idle:
        guidance.headline = "Ready";
        guidance.explanation = "Start a jam when you are.";
        break;

    case SessionPhase::CheckingAudio:
        if (!evidence.audioDevicesPresent) {
            guidance.headline = "No audio device";
            guidance.explanation =
                "Connect your interface or headphones, and JamLink will pick it up.";
            guidance.action = GuidanceAction::ReconnectAudioDevice;
            guidance.actionLabel = "Check devices";
        } else if (!evidence.soundCheckVerified) {
            guidance.headline = "Check your sound";
            guidance.explanation =
                "Play and sing at the level you actually will, so JamLink can "
                "confirm nothing is overloading.";
            guidance.action = GuidanceAction::FinishSoundCheck;
            guidance.actionLabel = "Sound Check";
        } else {
            guidance.headline = "Starting audio";
            guidance.explanation = "Opening your devices.";
        }
        guidance.actionEnabled = guidance.action != GuidanceAction::None;
        guidance.posture = RecoveryPosture::NeedsMusician;
        break;

    case SessionPhase::CheckingNetwork:
        if (evidence.firewallBlocking) {
            guidance.headline = "Windows is blocking JamLink";
            guidance.explanation =
                "Your friend's audio cannot reach this computer until JamLink is "
                "allowed through.";
            guidance.action = GuidanceAction::FixFirewall;
            guidance.actionLabel = "Fix Firewall";
        } else if (evidence.canJoinButNotHost || evidence.portMappingRefused) {
            guidance.headline = "Can't receive a connection here";
            guidance.explanation =
                "Your router will not open a port, so an invite from you is "
                "unlikely to work. Your friend can invite you instead.";
            guidance.action = GuidanceAction::AskFriendToHost;
            guidance.actionLabel = "Ask friend to host";
        } else {
            guidance.headline = "Getting ready";
            guidance.explanation = "Checking what this network allows.";
        }
        guidance.actionEnabled = guidance.action != GuidanceAction::None;
        guidance.posture = guidance.action == GuidanceAction::None
            ? RecoveryPosture::Compensating : RecoveryPosture::NeedsMusician;
        break;

    case SessionPhase::ReadyToInvite:
        guidance.headline = "Ready to invite";
        guidance.explanation = "Send one code to your friend and keep JamLink open.";
        guidance.action = GuidanceAction::SendInvite;
        guidance.actionLabel = "Copy Invite";
        guidance.actionEnabled = true;
        break;

    case SessionPhase::WaitingForPeer:
        guidance.headline = "Waiting for your friend";
        guidance.explanation = "They join with the code you sent.";
        guidance.action = GuidanceAction::WaitForFriend;
        guidance.actionLabel = "Copy Invite";
        guidance.actionEnabled = true;
        break;

    case SessionPhase::Joining:
        guidance.headline = "Joining";
        guidance.explanation = "Reaching your friend's room.";
        guidance.posture = RecoveryPosture::Compensating;
        break;

    case SessionPhase::Connecting:
        guidance.headline = "Almost there";
        guidance.explanation =
            "Connected, waiting for audio to start moving between you.";
        guidance.posture = RecoveryPosture::Compensating;
        break;

    case SessionPhase::ReadyToPlay:
        guidance.headline = "Ready to play";
        guidance.explanation = evidence.recording
            ? "Recording." : "You are connected and the audio is healthy.";
        guidance.playable = true;
        break;

    case SessionPhase::Degraded:
        guidance.headline = "Connection struggling";
        guidance.explanation =
            "JamLink is adding a little buffering to keep the audio stable. "
            "You can keep playing.";
        guidance.playable = true;
        guidance.posture = RecoveryPosture::Compensating;
        break;

    case SessionPhase::Reconnecting:
        guidance.headline = "Reconnecting";
        guidance.explanation =
            "The connection dropped. JamLink is restoring the room, and your "
            "settings are kept.";
        guidance.posture = RecoveryPosture::Reconnecting;
        break;

    case SessionPhase::FinalizingRecording:
        guidance.headline = "Finishing your take";
        guidance.explanation = "Writing the recording safely. This is quick.";
        guidance.posture = RecoveryPosture::Compensating;
        break;

    case SessionPhase::Completed:
        guidance.headline = "Session ended";
        guidance.explanation = "Everything was closed cleanly.";
        break;

    case SessionPhase::FailedRecoverable:
        if (evidence.recordingNeedsReview) {
            guidance.headline = "Recovered recording";
            guidance.explanation = "This take was interrupted and needs review.";
            guidance.action = GuidanceAction::ReviewRecording;
            guidance.actionLabel = "Review take";
        } else if (evidence.audioDeviceLost) {
            guidance.headline = "Audio interface disconnected";
            guidance.explanation =
                "Reconnect it and JamLink will try to restore your session "
                "automatically.";
            guidance.action = GuidanceAction::ReconnectAudioDevice;
            guidance.actionLabel = "Retry";
        } else {
            guidance.headline = "Couldn't keep the connection";
            guidance.explanation = "It may work on a second attempt.";
            guidance.action = GuidanceAction::Retry;
            guidance.actionLabel = "Try Again";
        }
        guidance.actionEnabled = true;
        guidance.posture = RecoveryPosture::NeedsMusician;
        break;

    case SessionPhase::FailedFinal:
        guidance.headline = "Different JamLink versions";
        guidance.explanation =
            "You and your friend need the same version before you can play.";
        guidance.action = GuidanceAction::LeaveRoom;
        guidance.actionLabel = "Close";
        guidance.actionEnabled = true;
        guidance.posture = RecoveryPosture::GaveUp;
        break;
    }

    // One most useful thing. A clipping input or a struggling device matters,
    // but not more than being unable to play at all, so it only speaks once the
    // session is otherwise healthy.
    if (guidance.playable && guidance.action == GuidanceAction::None) {
        if (evidence.inputClipping) {
            guidance.headline = "Input is overloading";
            guidance.explanation =
                "Turn the gain down on your interface, then clear the warning.";
            guidance.action = GuidanceAction::LowerInputGain;
            guidance.actionLabel = "Show me";
            guidance.actionEnabled = true;
            guidance.posture = RecoveryPosture::NeedsMusician;
        } else if (evidence.recentAudioDropouts != 0U) {
            guidance.headline = "Your computer is dropping audio";
            guidance.explanation =
                "A larger buffer size will steady it. This is your machine, not "
                "the connection.";
            guidance.action = GuidanceAction::ChooseLargerBuffer;
            guidance.actionLabel = "Open audio settings";
            guidance.actionEnabled = true;
            guidance.posture = RecoveryPosture::NeedsMusician;
        }
    }
    return guidance;
}

MusicianGuidance SessionConductor::update(
    const SessionEvidence& evidence, std::uint64_t atMillisecond) noexcept {
    const SessionPhase next = evaluate(evidence);
    if (next != phase_) {
        record(next, atMillisecond);
        phase_ = next;
    }
    guidance_ = guidanceFor(phase_, evidence);
    return guidance_;
}

void SessionConductor::record(SessionPhase next, std::uint64_t atMillisecond) noexcept {
    const PhaseTransition transition{phase_, next, atMillisecond};
    if (historyCount_ < historyCapacity) {
        history_[(historyStart_ + historyCount_) % historyCapacity] = transition;
        ++historyCount_;
        return;
    }
    // Full: drop the oldest. What a session is doing now is more useful for
    // support than how it began.
    history_[historyStart_] = transition;
    historyStart_ = (historyStart_ + 1U) % historyCapacity;
}

PhaseTransition SessionConductor::transitionAt(std::size_t index) const noexcept {
    if (index >= historyCount_) {
        return {};
    }
    return history_[(historyStart_ + index) % historyCapacity];
}

void SessionConductor::reset() noexcept {
    phase_ = SessionPhase::Idle;
    guidance_ = {};
    historyStart_ = 0U;
    historyCount_ = 0U;
}

} // namespace jamlink::control
