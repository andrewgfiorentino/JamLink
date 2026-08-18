// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/network/ice_agent.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace jamlink::network {

// Who is in the room, and who each musician therefore has to reach.
//
// A duo needs none of this: one person creates an invite, the other uses it,
// and both ends know exactly who they are talking to. A mesh does. If Andrew
// hosts and Mike and Sam each join him, Mike and Sam are in the same room and
// have never heard of each other -- and in a mesh they must be connected
// directly, because that is what a mesh is.
//
// Somebody therefore has to introduce them. The person who created the room is
// the only participant who is guaranteed to know everyone, so introductions
// come from there. That has a consequence worth stating rather than
// discovering: if the room's creator leaves, existing pairs keep playing but
// nobody new can be introduced. A duo has the same property today and nobody
// notices, because there is nobody left to introduce.
//
// The other thing a mesh needs is an answer to "which of us connects to which".
// Every pair has to agree, without asking each other, which end plays the part
// the host plays today -- the direction keys and the handshake both depend on
// it. Getting that wrong in either direction is a session that never forms:
// both waiting, or both calling.
enum class PairRole : std::uint8_t {
    // This end creates the room-side half of the pair and waits.
    Host,
    // This end reaches out.
    Guest,
    // The entry is this musician. Nobody connects to themselves.
    Self,
    // The two identifiers are indistinguishable, so no rule can separate them.
    // Reported rather than guessed: guessing produces two hosts or two guests,
    // and both of those are a session that silently never forms.
    Undecidable,
};

[[nodiscard]] std::string_view pairRoleName(PairRole role) noexcept;

struct RosterMember final {
    // Authenticated at the handshake. Self-asserted before that, which is why
    // this is never used as a secret -- only as a stable name to sort by.
    std::string participantId;
    std::string displayName;
    // Everywhere this musician might be reachable, in the same form an invite
    // carries. Replaced wholesale when they reconnect from somewhere new.
    std::vector<IceCandidate> candidates;
};

// One peer this musician has to establish a path to, and which end of it to be.
struct PendingPeer final {
    RosterMember member;
    PairRole role{PairRole::Guest};
};

class RoomRoster final {
public:
    // Bounded to what a mesh is meant to carry. Beyond this the number of paths
    // grows faster than anyone's connection does.
    static constexpr std::size_t maximumMembers = 6U;

    // Adds a musician, or replaces what is known about one already here.
    //
    // Replacing rather than appending is the point: somebody who drops and
    // comes back has new addresses, and leaving the old ones behind would have
    // everyone still probing somewhere nobody is.
    //
    // Returns false when the room is full or the entry is unusable, so a caller
    // can say so rather than silently ignoring somebody.
    [[nodiscard]] bool remember(const RosterMember& member);

    // Returns false when there was nobody by that name, which is not an error
    // -- a leave notice can legitimately arrive twice.
    bool forget(const std::string& participantId);

    void clear() noexcept;

    [[nodiscard]] const std::vector<RosterMember>& members() const noexcept {
        return members_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return members_.size(); }
    [[nodiscard]] bool contains(const std::string& participantId) const noexcept;

    // Which end of a pair to be. Symmetric by construction: both musicians run
    // the same comparison over the same two names and reach opposite answers,
    // without exchanging anything.
    [[nodiscard]] static PairRole roleToward(
        const std::string& localId, const std::string& remoteId) noexcept;

    // Everyone this musician still has to reach, with the part to play for
    // each. Excludes themselves and anyone already connected.
    [[nodiscard]] std::vector<PendingPeer> peersToConnect(
        const std::string& localId,
        const std::vector<std::string>& alreadyConnected) const;

private:
    std::vector<RosterMember> members_;
};

} // namespace jamlink::network
