// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace jamlink::control {

enum class AdmissionState : std::uint8_t {
    Waiting,
    Admitted,
    Denied,
    Disconnected
};

enum class RoomRole : std::uint8_t {
    Waiting,
    Listener,
    VoiceGuest,
    Performer,
    CoHost,
    Host
};

enum class RoomCapability : std::uint8_t {
    HearRoom,
    ReadChat,
    SendChat,
    SendVoice,
    SendMusic,
    RequestPerform,
    ControlRoomRecord,
    AdmitUsers,
    RemoveUsers,
    GrantPerformer,
    RevokePerformer,
    LockRoom,
    ChangeVisibility,
    ManageRoom,
    EndRoom,
    Count
};

using CapabilitySet = std::uint32_t;

[[nodiscard]] constexpr bool validCapability(RoomCapability capability) noexcept {
    return static_cast<std::uint8_t>(capability)
        < static_cast<std::uint8_t>(RoomCapability::Count);
}

[[nodiscard]] constexpr CapabilitySet capabilityBit(RoomCapability capability) noexcept {
    return validCapability(capability)
        ? CapabilitySet{1U} << static_cast<std::uint8_t>(capability)
        : CapabilitySet{0U};
}

[[nodiscard]] constexpr bool hasCapability(
    CapabilitySet capabilities,
    RoomCapability capability) noexcept {
    return validCapability(capability)
        && (capabilities & capabilityBit(capability)) != 0U;
}

[[nodiscard]] CapabilitySet roleCapabilities(RoomRole role) noexcept;

struct RoomParticipantState final {
    std::string participantId;
    std::string displayName;
    RoomRole role{RoomRole::Waiting};
    AdmissionState admission{AdmissionState::Waiting};
    CapabilitySet capabilities{0U};
    std::uint64_t grantRevision{0U};
    std::uint64_t resumeExpiresAtMilliseconds{0U};
    std::string resumeToken;
};

// Control-thread-only authoritative room state. It has bounded storage and no
// dependency on audio, sockets, UI, or the realtime callback. Network workers
// consume its decisions and publish only atomic media gates to callbacks.
class RoomAuthority final {
public:
    static constexpr std::size_t maximumParticipants = 12U;

    RoomAuthority(std::string hostParticipantId, std::string hostDisplayName);

    [[nodiscard]] bool requestAdmission(
        std::string participantId,
        std::string displayName);
    [[nodiscard]] bool admit(
        std::string_view actorId,
        std::string_view participantId,
        RoomRole initialRole) noexcept;
    [[nodiscard]] bool deny(
        std::string_view actorId,
        std::string_view participantId) noexcept;
    [[nodiscard]] bool setRole(
        std::string_view actorId,
        std::string_view participantId,
        RoomRole role) noexcept;
    [[nodiscard]] bool setCapability(
        std::string_view actorId,
        std::string_view participantId,
        RoomCapability capability,
        bool enabled) noexcept;
    [[nodiscard]] bool disconnect(
        std::string_view participantId,
        std::string resumeToken,
        std::uint64_t expiresAtMilliseconds);
    [[nodiscard]] bool resume(
        std::string_view participantId,
        std::string_view resumeToken,
        std::uint64_t nowMilliseconds,
        std::string replacementToken,
        std::uint64_t replacementExpiresAtMilliseconds);
    void reapInactive(std::uint64_t nowMilliseconds) noexcept;

    [[nodiscard]] bool authorize(
        std::string_view participantId,
        RoomCapability capability) const noexcept;
    [[nodiscard]] const RoomParticipantState* find(
        std::string_view participantId) const noexcept;
    [[nodiscard]] std::size_t participantCount() const noexcept;
    [[nodiscard]] std::size_t waitingCount() const noexcept;

private:
    [[nodiscard]] RoomParticipantState* findMutable(std::string_view participantId) noexcept;
    [[nodiscard]] static bool mayManageRole(
        const RoomParticipantState& actor,
        const RoomParticipantState& target,
        RoomRole requested) noexcept;
    void bumpGrant(RoomParticipantState& participant) noexcept;
    void removeAt(std::size_t index) noexcept;

    std::array<RoomParticipantState, maximumParticipants> participants_{};
    std::size_t participantCount_{0U};
    std::uint64_t nextGrantRevision_{1U};
};

} // namespace jamlink::control
