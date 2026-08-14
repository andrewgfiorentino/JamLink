// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace jamlink::network {

// These are control-thread diagnostics. They describe what JamLink learned
// while preparing a direct invitation; they are not end-to-end measurements.
enum class PublicAddressDiscoveryState : std::uint8_t {
    NotAttempted,
    Succeeded,
    Failed
};

enum class PortMappingState : std::uint8_t {
    NotRequested,
    Succeeded,
    Failed
};

enum class ReachabilityAssessment : std::uint8_t {
    Unknown,
    LikelyReachable,
    LikelyBlocked,
    RelayRequired
};

enum class ConnectionPreflightOutcome : std::uint8_t {
    NotRun,
    Ready,
    DirectMayNeedHelp,
    RelayRequired,
    Blocked
};

enum class ConnectionPreflightAction : std::uint8_t {
    None,
    FinishSoundCheck,
    UseCurrentBuild,
    ChooseAnotherUdpPort,
    EnableMappingOrForwardPort,
    CheckFirewall,
    ConfigureRelay
};

struct ConnectionPreflightChecks final {
    bool audioReady{false};
    bool buildIdentityReady{false};
    bool protocolIdentityReady{false};
    bool udpBindSucceeded{false};
    PublicAddressDiscoveryState publicAddress{
        PublicAddressDiscoveryState::NotAttempted};
    PortMappingState portMapping{PortMappingState::NotRequested};
    ReachabilityAssessment reachability{ReachabilityAssessment::Unknown};
};

struct ConnectionPreflightResult final {
    ConnectionPreflightOutcome outcome{ConnectionPreflightOutcome::NotRun};
    ConnectionPreflightAction action{ConnectionPreflightAction::None};
    // A warning does not suppress the infrastructure-independent invite. It
    // tells the host that manual forwarding or a future relay may be needed.
    bool directInviteAllowed{false};
};

[[nodiscard]] ConnectionPreflightResult evaluateConnectionPreflight(
    const ConnectionPreflightChecks& checks) noexcept;

} // namespace jamlink::network
