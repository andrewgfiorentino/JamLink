// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/control/room_capacity.hpp"

#include <algorithm>

namespace jamlink::control {
namespace {

// In a mesh a musician sends one copy of everything to each of the others, so
// what this machine has to push is decided by how many other people are here.
[[nodiscard]] std::uint64_t upstreamFor(
    std::uint32_t participants, const RoomCapacityEvidence& evidence) noexcept {
    if (participants < 2U) {
        return 0U;
    }
    return static_cast<std::uint64_t>(participants - 1U)
        * static_cast<std::uint64_t>(evidence.streamsPerParticipant)
        * static_cast<std::uint64_t>(evidence.bitsPerSecondPerStream);
}

} // namespace

std::string_view capacityVerdictName(RoomCapacityVerdict verdict) noexcept {
    switch (verdict) {
    case RoomCapacityVerdict::Comfortable: return "Comfortable";
    case RoomCapacityVerdict::Unverified: return "Unverified";
    case RoomCapacityVerdict::WouldExceedUplink: return "WouldExceedUplink";
    case RoomCapacityVerdict::AlreadyStrained: return "AlreadyStrained";
    case RoomCapacityVerdict::BeyondMeshLimit: return "BeyondMeshLimit";
    }
    return "Unknown";
}

std::string_view RoomCapacityResult::advice() const noexcept {
    switch (verdict) {
    case RoomCapacityVerdict::Comfortable:
    case RoomCapacityVerdict::Unverified:
        // Saying nothing is right for both. One has nothing to warn about and
        // the other has nothing to say -- and a warning that fires whenever
        // JamLink is merely unsure is a warning people learn to dismiss.
        return "";
    case RoomCapacityVerdict::WouldExceedUplink:
        // Never "insufficient upstream bandwidth". What a musician can act on
        // is who else is in the room.
        return "Everyone here sends their audio to everyone else, so each extra "
               "musician costs you more upload. One more would be more than your "
               "connection has said it can carry, and it would break up for "
               "everybody rather than just for them.";
    case RoomCapacityVerdict::AlreadyStrained:
        // Measured, so this is stated plainly rather than hedged.
        return "Your connection is already sending less than it would like to for "
               "the people here. Adding another musician would make that worse for "
               "everyone, so it is worth waiting until this settles.";
    case RoomCapacityVerdict::BeyondMeshLimit:
        return "This is as many musicians as a direct session is built for. "
               "Everyone sends to everyone, so each extra person costs every other "
               "person more, and past this it stops being playable.";
    }
    return "";
}

RoomCapacityResult evaluateRoomCapacity(const RoomCapacityEvidence& evidence) noexcept {
    RoomCapacityResult result;
    const std::uint32_t participants = std::max<std::uint32_t>(evidence.participants, 1U);
    result.currentUpstreamBitsPerSecond = upstreamFor(participants, evidence);
    result.upstreamWithOneMoreBitsPerSecond = upstreamFor(participants + 1U, evidence);

    if (participants >= maximumMeshParticipants) {
        result.verdict = RoomCapacityVerdict::BeyondMeshLimit;
        result.canAdmitAnother = false;
        return result;
    }

    // Measured strain outranks arithmetic. The connection has already answered
    // the question for the people who are here, and no sum about a figure the
    // musician typed in is going to overrule what it is actually delivering.
    if (evidence.sendRateAlreadyReduced) {
        result.verdict = RoomCapacityVerdict::AlreadyStrained;
        result.canAdmitAnother = false;
        return result;
    }

    if (evidence.declaredUplinkBitsPerSecond == 0U) {
        // Nobody knows their upload speed, and the figure an operator
        // advertises is not what a house gets in the evening. Unverified is
        // the honest answer, and it does not block anyone: refusing a musician
        // on the strength of a number we never had would be worse than letting
        // them in and adapting.
        result.verdict = RoomCapacityVerdict::Unverified;
        result.canAdmitAnother = true;
        return result;
    }

    const auto usable = static_cast<std::uint64_t>(
        static_cast<double>(evidence.declaredUplinkBitsPerSecond)
        * (1.0 - uplinkHeadroomFraction));
    if (result.upstreamWithOneMoreBitsPerSecond > usable) {
        result.verdict = RoomCapacityVerdict::WouldExceedUplink;
        result.canAdmitAnother = false;
        return result;
    }

    result.verdict = RoomCapacityVerdict::Comfortable;
    result.canAdmitAnother = true;
    return result;
}

} // namespace jamlink::control
