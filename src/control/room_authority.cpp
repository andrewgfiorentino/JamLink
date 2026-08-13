// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/control/room_authority.hpp"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace jamlink::control {
namespace {

constexpr CapabilitySet caps(std::initializer_list<RoomCapability> values) noexcept {
    CapabilitySet result = 0U;
    for (const RoomCapability value : values) {
        result |= capabilityBit(value);
    }
    return result;
}

bool constantTimeEqual(std::string_view left, std::string_view right) noexcept {
    const std::size_t compared = std::max(left.size(), right.size());
    std::size_t difference = left.size() ^ right.size();
    for (std::size_t index = 0U; index < compared; ++index) {
        const unsigned char leftByte = index < left.size()
            ? static_cast<unsigned char>(left[index]) : 0U;
        const unsigned char rightByte = index < right.size()
            ? static_cast<unsigned char>(right[index]) : 0U;
        difference |= static_cast<std::size_t>(leftByte ^ rightByte);
    }
    return difference == 0U;
}

} // namespace

CapabilitySet roleCapabilities(RoomRole role) noexcept {
    switch (role) {
    case RoomRole::Waiting:
        return 0U;
    case RoomRole::Listener:
        return caps({RoomCapability::HearRoom, RoomCapability::ReadChat,
            RoomCapability::RequestPerform});
    case RoomRole::VoiceGuest:
        return roleCapabilities(RoomRole::Listener)
            | caps({RoomCapability::SendChat, RoomCapability::SendVoice});
    case RoomRole::Performer:
        return roleCapabilities(RoomRole::VoiceGuest)
            | capabilityBit(RoomCapability::SendMusic);
    case RoomRole::CoHost:
        return roleCapabilities(RoomRole::Performer)
            | caps({RoomCapability::AdmitUsers, RoomCapability::RemoveUsers,
                RoomCapability::GrantPerformer, RoomCapability::RevokePerformer,
                RoomCapability::LockRoom});
    case RoomRole::Host:
        return (capabilityBit(RoomCapability::Count) - 1U);
    }
    return 0U;
}

RoomAuthority::RoomAuthority(
    std::string hostParticipantId,
    std::string hostDisplayName) {
    if (hostParticipantId.empty()) {
        return;
    }
    RoomParticipantState& host = participants_[0U];
    host.participantId = std::move(hostParticipantId);
    host.displayName = std::move(hostDisplayName);
    host.role = RoomRole::Host;
    host.admission = AdmissionState::Admitted;
    host.capabilities = roleCapabilities(RoomRole::Host);
    host.grantRevision = nextGrantRevision_++;
    participantCount_ = 1U;
}

bool RoomAuthority::requestAdmission(
    std::string participantId,
    std::string displayName) {
    const auto reusable = std::find_if(
        participants_.begin() + static_cast<std::ptrdiff_t>(1U),
        participants_.begin() + static_cast<std::ptrdiff_t>(participantCount_),
        [](const RoomParticipantState& participant) {
            return participant.admission == AdmissionState::Denied;
        });
    if (reusable != participants_.begin() + static_cast<std::ptrdiff_t>(participantCount_)) {
        removeAt(static_cast<std::size_t>(std::distance(participants_.begin(), reusable)));
    }
    if (participantId.empty() || participantId.size() > 128U
        || displayName.empty() || displayName.size() > 64U
        || participantCount_ >= participants_.size()
        || find(participantId) != nullptr) {
        return false;
    }
    RoomParticipantState& participant = participants_[participantCount_++];
    participant.participantId = std::move(participantId);
    participant.displayName = std::move(displayName);
    participant.role = RoomRole::Waiting;
    participant.admission = AdmissionState::Waiting;
    participant.capabilities = 0U;
    participant.grantRevision = nextGrantRevision_++;
    return true;
}

bool RoomAuthority::admit(
    std::string_view actorId,
    std::string_view participantId,
    RoomRole initialRole) noexcept {
    RoomParticipantState* actor = findMutable(actorId);
    RoomParticipantState* participant = findMutable(participantId);
    if (actor == nullptr || participant == nullptr
        || participant->admission != AdmissionState::Waiting
        || initialRole == RoomRole::Waiting || initialRole == RoomRole::Host
        || !hasCapability(actor->capabilities, RoomCapability::AdmitUsers)
        || (initialRole == RoomRole::Performer
            && !hasCapability(actor->capabilities, RoomCapability::GrantPerformer))
        || !mayManageRole(*actor, *participant, initialRole)) {
        return false;
    }
    participant->admission = AdmissionState::Admitted;
    participant->role = initialRole;
    participant->capabilities = roleCapabilities(initialRole);
    participant->resumeToken.clear();
    participant->resumeExpiresAtMilliseconds = 0U;
    bumpGrant(*participant);
    return true;
}

bool RoomAuthority::deny(
    std::string_view actorId,
    std::string_view participantId) noexcept {
    RoomParticipantState* actor = findMutable(actorId);
    RoomParticipantState* participant = findMutable(participantId);
    if (actor == nullptr || participant == nullptr
        || participant->admission != AdmissionState::Waiting
        || !hasCapability(actor->capabilities, RoomCapability::AdmitUsers)) {
        return false;
    }
    participant->admission = AdmissionState::Denied;
    participant->capabilities = 0U;
    bumpGrant(*participant);
    return true;
}

bool RoomAuthority::setRole(
    std::string_view actorId,
    std::string_view participantId,
    RoomRole role) noexcept {
    RoomParticipantState* actor = findMutable(actorId);
    RoomParticipantState* participant = findMutable(participantId);
    if (actor == nullptr || participant == nullptr
        || participant->admission != AdmissionState::Admitted
        || role == RoomRole::Waiting || role == RoomRole::Host
        || !mayManageRole(*actor, *participant, role)) {
        return false;
    }
    if (role == RoomRole::Performer
        && !hasCapability(actor->capabilities, RoomCapability::GrantPerformer)) {
        return false;
    }
    if (participant->role == RoomRole::Performer && role != RoomRole::Performer
        && !hasCapability(actor->capabilities, RoomCapability::RevokePerformer)) {
        return false;
    }
    participant->role = role;
    participant->capabilities = roleCapabilities(role);
    bumpGrant(*participant);
    return true;
}

bool RoomAuthority::setCapability(
    std::string_view actorId,
    std::string_view participantId,
    RoomCapability capability,
    bool enabled) noexcept {
    RoomParticipantState* actor = findMutable(actorId);
    RoomParticipantState* participant = findMutable(participantId);
    if (actor == nullptr || participant == nullptr
        || participant->admission != AdmissionState::Admitted
        || participant->role == RoomRole::Host
        || !hasCapability(actor->capabilities, RoomCapability::ManageRoom)
        || !validCapability(capability) || capability == RoomCapability::EndRoom) {
        return false;
    }
    if (enabled) {
        participant->capabilities |= capabilityBit(capability);
    } else {
        participant->capabilities &= ~capabilityBit(capability);
    }
    bumpGrant(*participant);
    return true;
}

bool RoomAuthority::disconnect(
    std::string_view participantId,
    std::string resumeToken,
    std::uint64_t expiresAtMilliseconds) {
    RoomParticipantState* participant = findMutable(participantId);
    if (participant == nullptr || participant->admission != AdmissionState::Admitted
        || participant->role == RoomRole::Host || resumeToken.size() < 32U
        || resumeToken.size() > 128U
        || expiresAtMilliseconds == 0U) {
        return false;
    }
    participant->admission = AdmissionState::Disconnected;
    participant->resumeToken = std::move(resumeToken);
    participant->resumeExpiresAtMilliseconds = expiresAtMilliseconds;
    bumpGrant(*participant);
    return true;
}

bool RoomAuthority::resume(
    std::string_view participantId,
    std::string_view resumeToken,
    std::uint64_t nowMilliseconds,
    std::string replacementToken,
    std::uint64_t replacementExpiresAtMilliseconds) {
    RoomParticipantState* participant = findMutable(participantId);
    if (participant == nullptr || participant->admission != AdmissionState::Disconnected
        || participant->resumeToken.empty()
        || resumeToken.size() < 32U || resumeToken.size() > 128U
        || !constantTimeEqual(participant->resumeToken, resumeToken)
        || nowMilliseconds >= participant->resumeExpiresAtMilliseconds
        || replacementToken.size() < 32U || replacementToken.size() > 128U
        || replacementExpiresAtMilliseconds <= nowMilliseconds) {
        return false;
    }
    participant->admission = AdmissionState::Admitted;
    participant->resumeToken = std::move(replacementToken);
    participant->resumeExpiresAtMilliseconds = replacementExpiresAtMilliseconds;
    bumpGrant(*participant);
    return true;
}

void RoomAuthority::reapInactive(std::uint64_t nowMilliseconds) noexcept {
    std::size_t index = 1U;
    while (index < participantCount_) {
        const RoomParticipantState& participant = participants_[index];
        const bool remove = participant.admission == AdmissionState::Denied
            || (participant.admission == AdmissionState::Disconnected
                && nowMilliseconds >= participant.resumeExpiresAtMilliseconds);
        if (remove) {
            removeAt(index);
        } else {
            ++index;
        }
    }
}

bool RoomAuthority::authorize(
    std::string_view participantId,
    RoomCapability capability) const noexcept {
    const RoomParticipantState* participant = find(participantId);
    return participant != nullptr
        && participant->admission == AdmissionState::Admitted
        && validCapability(capability)
        && hasCapability(participant->capabilities, capability);
}

const RoomParticipantState* RoomAuthority::find(
    std::string_view participantId) const noexcept {
    const auto end = participants_.begin() + static_cast<std::ptrdiff_t>(participantCount_);
    const auto found = std::find_if(participants_.begin(), end,
        [participantId](const RoomParticipantState& participant) {
            return participant.participantId == participantId;
        });
    return found == end ? nullptr : &*found;
}

std::size_t RoomAuthority::participantCount() const noexcept { return participantCount_; }

std::size_t RoomAuthority::waitingCount() const noexcept {
    const auto end = participants_.begin() + static_cast<std::ptrdiff_t>(participantCount_);
    return static_cast<std::size_t>(std::count_if(participants_.begin(), end,
        [](const RoomParticipantState& participant) {
            return participant.admission == AdmissionState::Waiting;
        }));
}

RoomParticipantState* RoomAuthority::findMutable(std::string_view participantId) noexcept {
    return const_cast<RoomParticipantState*>(std::as_const(*this).find(participantId));
}

bool RoomAuthority::mayManageRole(
    const RoomParticipantState& actor,
    const RoomParticipantState& target,
    RoomRole requested) noexcept {
    if (actor.admission != AdmissionState::Admitted || actor.participantId == target.participantId
        || target.role == RoomRole::Host || requested == RoomRole::Host) {
        return false;
    }
    return actor.role == RoomRole::Host
        || (actor.role == RoomRole::CoHost
            && target.role != RoomRole::CoHost
            && requested != RoomRole::CoHost);
}

void RoomAuthority::bumpGrant(RoomParticipantState& participant) noexcept {
    participant.grantRevision = nextGrantRevision_++;
}

void RoomAuthority::removeAt(std::size_t index) noexcept {
    if (index == 0U || index >= participantCount_) {
        return;
    }
    for (std::size_t current = index; current + 1U < participantCount_; ++current) {
        participants_[current] = std::move(participants_[current + 1U]);
    }
    participants_[participantCount_ - 1U] = RoomParticipantState{};
    --participantCount_;
}

} // namespace jamlink::control
