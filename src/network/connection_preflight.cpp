// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/connection_preflight.hpp"

namespace jamlink::network {

ConnectionPreflightResult evaluateConnectionPreflight(
    const ConnectionPreflightChecks& checks) noexcept {
    if (!checks.audioReady) {
        return {ConnectionPreflightOutcome::Blocked,
                ConnectionPreflightAction::FinishSoundCheck, false};
    }
    if (!checks.buildIdentityReady || !checks.protocolIdentityReady) {
        return {ConnectionPreflightOutcome::Blocked,
                ConnectionPreflightAction::UseCurrentBuild, false};
    }
    if (!checks.udpBindSucceeded) {
        return {ConnectionPreflightOutcome::Blocked,
                ConnectionPreflightAction::ChooseAnotherUdpPort, false};
    }
    if (checks.reachability == ReachabilityAssessment::RelayRequired) {
        return {ConnectionPreflightOutcome::RelayRequired,
                ConnectionPreflightAction::ConfigureRelay, false};
    }
    if (checks.reachability == ReachabilityAssessment::LikelyBlocked) {
        return {ConnectionPreflightOutcome::DirectMayNeedHelp,
                ConnectionPreflightAction::CheckFirewall, true};
    }
    if (checks.publicAddress == PublicAddressDiscoveryState::Succeeded
        && checks.portMapping == PortMappingState::Succeeded
        && checks.reachability == ReachabilityAssessment::LikelyReachable) {
        return {ConnectionPreflightOutcome::Ready,
                ConnectionPreflightAction::None, true};
    }
    if (checks.portMapping != PortMappingState::Succeeded) {
        return {ConnectionPreflightOutcome::DirectMayNeedHelp,
                ConnectionPreflightAction::EnableMappingOrForwardPort, true};
    }
    return {ConnectionPreflightOutcome::DirectMayNeedHelp,
            ConnectionPreflightAction::CheckFirewall, true};
}

} // namespace jamlink::network
