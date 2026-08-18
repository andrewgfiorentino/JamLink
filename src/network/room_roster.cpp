// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/room_roster.hpp"

#include <algorithm>

namespace jamlink::network {

std::string_view pairRoleName(PairRole role) noexcept {
    switch (role) {
    case PairRole::Host: return "Host";
    case PairRole::Guest: return "Guest";
    case PairRole::Self: return "Self";
    case PairRole::Undecidable: return "Undecidable";
    }
    return "Unknown";
}

bool RoomRoster::contains(const std::string& participantId) const noexcept {
    return std::any_of(
        members_.begin(), members_.end(),
        [&participantId](const RosterMember& member) {
            return member.participantId == participantId;
        });
}

bool RoomRoster::remember(const RosterMember& member) {
    if (member.participantId.empty()) {
        // Without a name there is nothing to sort by, so no pair containing
        // this entry could ever agree which end to be.
        return false;
    }
    const auto existing = std::find_if(
        members_.begin(), members_.end(),
        [&member](const RosterMember& candidate) {
            return candidate.participantId == member.participantId;
        });
    if (existing != members_.end()) {
        // Replaced wholesale rather than merged. Somebody who dropped and came
        // back has new addresses, and keeping the old ones would leave everyone
        // probing somewhere nobody is.
        *existing = member;
        return true;
    }
    if (members_.size() >= maximumMembers) {
        return false;
    }
    members_.push_back(member);
    return true;
}

bool RoomRoster::forget(const std::string& participantId) {
    const auto found = std::find_if(
        members_.begin(), members_.end(),
        [&participantId](const RosterMember& member) {
            return member.participantId == participantId;
        });
    if (found == members_.end()) {
        // Not an error. A leave notice can legitimately arrive twice, once
        // from the person leaving and once from whoever noticed.
        return false;
    }
    members_.erase(found);
    return true;
}

void RoomRoster::clear() noexcept { members_.clear(); }

PairRole RoomRoster::roleToward(
    const std::string& localId, const std::string& remoteId) noexcept {
    if (localId.empty() || remoteId.empty()) {
        return PairRole::Undecidable;
    }
    if (localId == remoteId) {
        // Either this entry is us, or two musicians are claiming one identity.
        // Both mean there is no pair here to form: nobody connects to
        // themselves, and a copied identity must not be able to make somebody
        // start a second session with what looks like their own name.
        return PairRole::Self;
    }
    // Both ends run this same comparison over the same two names and reach
    // opposite answers without exchanging anything. Any rule would do provided
    // it is a total order; what matters is that it is the same rule on both
    // sides, because two hosts or two guests is a session that never forms and
    // reports nothing while not forming.
    return localId < remoteId ? PairRole::Host : PairRole::Guest;
}

std::vector<PendingPeer> RoomRoster::peersToConnect(
    const std::string& localId,
    const std::vector<std::string>& alreadyConnected) const {
    std::vector<PendingPeer> pending;
    pending.reserve(members_.size());
    for (const RosterMember& member : members_) {
        const PairRole role = roleToward(localId, member.participantId);
        if (role == PairRole::Self || role == PairRole::Undecidable) {
            continue;
        }
        if (std::find(alreadyConnected.begin(), alreadyConnected.end(),
                      member.participantId) != alreadyConnected.end()) {
            continue;
        }
        if (member.candidates.empty()) {
            // Announced but not yet reachable. Skipped rather than attempted,
            // because a peer with no addresses would otherwise be retried
            // forever against nothing.
            continue;
        }
        pending.push_back(PendingPeer{member, role});
    }
    return pending;
}

} // namespace jamlink::network
