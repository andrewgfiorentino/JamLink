// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>

namespace jamlink::control {

// How many musicians this connection can actually carry.
//
// JamLink is going to a full mesh: everyone sends their own audio directly to
// everyone else. That is the right choice for a small group -- no hub, nobody's
// machine carrying the room, no extra hop through a third party, and one
// person's connection failing takes out one path rather than the session. It
// has one property that has to be designed around rather than discovered:
//
//   Upstream scales with the number of other people. Two streams at the
//   current rate is roughly 320 kbit/s to each of them, so a fourth musician
//   costs every existing musician another 320 kbit/s of upload, not just the
//   person joining.
//
// A hub keeps upstream flat and was the alternative. It was not chosen, so this
// exists: the failure mode of mesh is that one more person joins and everybody
// gets worse at once, with nothing on screen connecting the two. Somebody who
// added a fifth person would conclude JamLink is unreliable, and they would be
// describing the arithmetic rather than a defect.
//
// Deciding this from arithmetic alone would be a guess, because almost nobody
// knows their upload speed and the figure an operator advertises is not what a
// house gets at nine in the evening. So measured strain outranks arithmetic:
// if the send rate has already had to be reduced for the people who are here,
// the connection has answered the question and no sum is going to overrule it.
enum class RoomCapacityVerdict : std::uint8_t {
    // Room to spare, and something real says so.
    Comfortable,
    // Nothing indicates a problem, but nothing has confirmed there is room
    // either. Never presented as a promise.
    Unverified,
    // Arithmetic says one more will not fit in the declared uplink.
    WouldExceedUplink,
    // The connection is already being asked for more than it is delivering.
    // Measured, and therefore final.
    AlreadyStrained,
    // Beyond what a mesh of this shape is meant to do at all.
    BeyondMeshLimit,
};

[[nodiscard]] std::string_view capacityVerdictName(RoomCapacityVerdict verdict) noexcept;

struct RoomCapacityEvidence final {
    // People in the room including this one. A duo is two.
    std::uint32_t participants{2U};
    // Streams each musician sends. Guitar and voice today.
    std::uint32_t streamsPerParticipant{2U};
    // What one stream currently costs, which the bitrate controller may
    // already have lowered.
    std::uint32_t bitsPerSecondPerStream{96'000U};
    // What the musician has told JamLink their upload is. Zero means they have
    // not, which is the common case and must not be treated as zero bandwidth.
    std::uint64_t declaredUplinkBitsPerSecond{0U};
    // The send rate has already had to be reduced because the far end reported
    // losing audio. This is the connection answering for itself.
    bool sendRateAlreadyReduced{false};
};

// Above this a mesh stops being the right shape regardless of bandwidth: the
// number of paths grows with the square of the group, and so does the number
// of ways for one of them to be the problem.
inline constexpr std::uint32_t maximumMeshParticipants = 6U;

// Headroom kept back. A link run to exactly its capacity has none for the
// bursts that real audio and real routers produce.
inline constexpr double uplinkHeadroomFraction = 0.35;

struct RoomCapacityResult final {
    RoomCapacityVerdict verdict{RoomCapacityVerdict::Unverified};
    // Whether one more musician can be admitted. False only when something
    // says so -- never merely because nothing has confirmed it.
    bool canAdmitAnother{true};
    // What this machine has to send right now, in bits per second. Mesh means
    // one copy to each of the others.
    std::uint64_t currentUpstreamBitsPerSecond{0U};
    // What it would have to send with one more person here.
    std::uint64_t upstreamWithOneMoreBitsPerSecond{0U};

    // Musician language. Empty when there is nothing worth saying.
    [[nodiscard]] std::string_view advice() const noexcept;
};

[[nodiscard]] RoomCapacityResult evaluateRoomCapacity(
    const RoomCapacityEvidence& evidence) noexcept;

} // namespace jamlink::control
