// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/control/room_authority.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void check(bool condition, std::string_view name) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << name << '\n';
    }
}

void completeRoleCapabilityMatrix() {
    using namespace jamlink::control;
    constexpr std::array roles{
        RoomRole::Waiting, RoomRole::Listener, RoomRole::VoiceGuest,
        RoomRole::Performer, RoomRole::CoHost, RoomRole::Host};
    const std::array expected{
        CapabilitySet{0U},
        capabilityBit(RoomCapability::HearRoom)
            | capabilityBit(RoomCapability::ReadChat)
            | capabilityBit(RoomCapability::RequestPerform),
        capabilityBit(RoomCapability::HearRoom)
            | capabilityBit(RoomCapability::ReadChat)
            | capabilityBit(RoomCapability::RequestPerform)
            | capabilityBit(RoomCapability::SendChat)
            | capabilityBit(RoomCapability::SendVoice),
        capabilityBit(RoomCapability::HearRoom)
            | capabilityBit(RoomCapability::ReadChat)
            | capabilityBit(RoomCapability::RequestPerform)
            | capabilityBit(RoomCapability::SendChat)
            | capabilityBit(RoomCapability::SendVoice)
            | capabilityBit(RoomCapability::SendMusic),
        roleCapabilities(RoomRole::Performer)
            | capabilityBit(RoomCapability::AdmitUsers)
            | capabilityBit(RoomCapability::RemoveUsers)
            | capabilityBit(RoomCapability::GrantPerformer)
            | capabilityBit(RoomCapability::RevokePerformer)
            | capabilityBit(RoomCapability::LockRoom),
        capabilityBit(RoomCapability::Count) - 1U};
    for (std::size_t roleIndex = 0U; roleIndex < roles.size(); ++roleIndex) {
        const CapabilitySet actual = roleCapabilities(roles[roleIndex]);
        check(actual == expected[roleIndex], "role preset exact capability set");
        for (std::uint8_t value = 0U;
             value < static_cast<std::uint8_t>(RoomCapability::Count); ++value) {
            const auto capability = static_cast<RoomCapability>(value);
            check(
                hasCapability(actual, capability)
                    == ((expected[roleIndex] & capabilityBit(capability)) != 0U),
                "role capability matrix");
        }
    }
    constexpr std::array invalidCapabilities{
        RoomCapability::Count,
        static_cast<RoomCapability>(31U),
        static_cast<RoomCapability>(32U),
        static_cast<RoomCapability>(255U)};
    for (const RoomCapability capability : invalidCapabilities) {
        check(!validCapability(capability), "invalid capability rejected");
        check(capabilityBit(capability) == 0U, "invalid capability has no bit");
        check(!hasCapability(~CapabilitySet{0U}, capability),
            "invalid capability cannot be present");
    }
}

void waitingAndPromotionAreAuthoritative() {
    using namespace jamlink::control;
    RoomAuthority room("andrew", "Andrew");
    check(room.requestAdmission("mike", "Mike"), "waiting request accepted");
    check(room.waitingCount() == 1U, "waiting count increments");
    check(!room.authorize("mike", RoomCapability::HearRoom), "waiting cannot hear");
    check(!room.authorize("mike", RoomCapability::SendVoice), "waiting cannot send voice");
    check(!room.authorize("mike", RoomCapability::SendMusic), "waiting cannot send music");
    check(!room.admit("mike", "mike", RoomRole::Performer), "waiting cannot self admit");
    check(room.admit("andrew", "mike", RoomRole::Listener), "host admits listener");
    check(room.authorize("mike", RoomCapability::HearRoom), "listener hears");
    check(!room.authorize("mike", RoomCapability::SendVoice), "listener voice denied");
    check(!room.authorize("mike", RoomCapability::SendMusic), "listener music denied");
    check(room.setRole("andrew", "mike", RoomRole::VoiceGuest), "voice promotion");
    check(room.authorize("mike", RoomCapability::SendVoice), "voice guest sends voice");
    check(!room.authorize("mike", RoomCapability::SendMusic), "voice guest music denied");
    check(room.setRole("andrew", "mike", RoomRole::Performer), "performer promotion");
    check(room.authorize("mike", RoomCapability::SendMusic), "performer sends music");
    check(room.setRole("andrew", "mike", RoomRole::VoiceGuest), "music revocation");
    check(!room.authorize("mike", RoomCapability::SendMusic), "revoked music stops");
    check(room.authorize("mike", RoomCapability::SendVoice), "voice remains after music revoke");
}

void recordingAndResumeFailClosed() {
    using namespace jamlink::control;
    RoomAuthority room("host", "Host");
    check(room.requestAdmission("cohost", "Co-host"), "co-host request");
    check(room.admit("host", "cohost", RoomRole::CoHost), "co-host admitted");
    check(!room.authorize("cohost", RoomCapability::ControlRoomRecord),
        "co-host has no recording permission by default");
    check(room.setCapability(
        "host", "cohost", RoomCapability::ControlRoomRecord, true),
        "host explicitly grants recording");
    check(room.authorize("cohost", RoomCapability::ControlRoomRecord),
        "granted co-host may record");
    check(!room.setCapability(
        "cohost", "cohost", RoomCapability::EndRoom, true),
        "co-host cannot grant itself room ownership");
    check(!room.setCapability(
        "host", "cohost", RoomCapability::Count, true),
        "capability sentinel cannot be granted");
    check(!room.authorize("cohost", static_cast<RoomCapability>(255U)),
        "malformed capability cannot authorize");

    constexpr std::string_view firstToken = "0123456789abcdef0123456789abcdef";
    constexpr std::string_view nextToken = "fedcba9876543210fedcba9876543210";
    const auto* beforeDisconnect = room.find("cohost");
    const std::uint64_t revisionBeforeDisconnect = beforeDisconnect == nullptr
        ? 0U : beforeDisconnect->grantRevision;
    check(room.disconnect("cohost", std::string(firstToken), 2'000U), "trusted disconnect");
    check(room.find("cohost") != nullptr
            && room.find("cohost")->grantRevision > revisionBeforeDisconnect,
        "disconnect invalidates published grant revision");
    check(!room.authorize("cohost", RoomCapability::SendVoice),
        "disconnected participant has no live capability");
    check(!room.resume("cohost", "wrong-token-000000000000000000000", 1'000U,
        std::string(nextToken), 3'000U), "invalid resume token rejected");
    check(!room.resume("cohost", std::string(129U, 'x'), 1'000U,
        std::string(nextToken), 3'000U), "oversized presented token rejected");
    check(room.resume("cohost", firstToken, 1'000U, std::string(nextToken), 3'000U),
        "valid resume restores authorization");
    check(!room.resume("cohost", firstToken, 1'100U, std::string(firstToken), 3'100U),
        "old token cannot replay");
    check(room.authorize("cohost", RoomCapability::ControlRoomRecord),
        "explicit grant survives valid resume");
    check(!room.disconnect("cohost", std::string(129U, 'x'), 4'000U),
        "oversized resume token rejected");

    check(room.requestAdmission("second", "Second"), "second co-host request");
    check(room.admit("host", "second", RoomRole::CoHost), "second co-host admitted");
    check(!room.setRole("cohost", "second", RoomRole::Listener),
        "co-host cannot demote a peer co-host");
    check(room.setCapability(
        "host", "cohost", RoomCapability::GrantPerformer, false),
        "host revokes co-host performer grant");
    check(room.requestAdmission("third", "Third"), "third performer request");
    check(!room.admit("cohost", "third", RoomRole::Performer),
        "co-host cannot bypass revoked performer grant during admission");
}

void inactiveSlotsAreReclaimed() {
    using namespace jamlink::control;
    RoomAuthority room("host", "Host");
    for (std::size_t index = 0U; index < 32U; ++index) {
        const std::string id = "denied-" + std::to_string(index);
        check(room.requestAdmission(id, "Waiting"), "denied slot request accepted");
        check(room.deny("host", id), "request denied");
    }
    check(room.participantCount() == 2U,
        "bounded authority reuses denied records without exhaustion");
    check(room.requestAdmission("resume", "Resume"), "disconnect candidate accepted");
    check(room.admit("host", "resume", RoomRole::Performer), "disconnect candidate admitted");
    check(room.disconnect("resume", std::string(32U, 'r'), 500U),
        "disconnect candidate retained during grace period");
    room.reapInactive(499U);
    check(room.find("resume") != nullptr, "unexpired resume record retained");
    room.reapInactive(500U);
    check(room.find("resume") == nullptr, "expired resume record reclaimed");
}

} // namespace

int main() {
    completeRoleCapabilityMatrix();
    waitingAndPromotionAreAuthoritative();
    recordingAndResumeFailClosed();
    inactiveSlotsAreReclaimed();
    if (failures == 0) {
        std::cout << "JamLink room authority tests passed\n";
        return EXIT_SUCCESS;
    }
    return EXIT_FAILURE;
}
