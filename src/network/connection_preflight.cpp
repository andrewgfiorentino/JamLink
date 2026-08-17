// Copyright (c) 2026 Andrew Fiorentino
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
        // A router that rewrites the external port cannot be reached by a
        // direct invite, and no relay exists in this build to work around it.
        // Saying "relay required" therefore left a musician with nothing to do,
        // when in fact only the ability to host was lost: this machine can
        // still join a room the other person opens.
        return {ConnectionPreflightOutcome::JoinOnly,
                ConnectionPreflightAction::AskFriendToHost, false};
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
    if (checks.portMapping == PortMappingState::Failed) {
        // The router was asked for a port and refused. Port forwarding is a
        // real remedy but not one most people can follow, and it is not needed:
        // with a single invite code only the host must be reachable. In field
        // testing a router granted a mapping one hour and refused it the next,
        // and the session connected without trouble the moment the other end
        // made the invite, so that is the instruction worth leading with.
        return {ConnectionPreflightOutcome::DirectMayNeedHelp,
                ConnectionPreflightAction::AskFriendToHost, true};
    }
    if (checks.portMapping != PortMappingState::Succeeded) {
        return {ConnectionPreflightOutcome::DirectMayNeedHelp,
                ConnectionPreflightAction::EnableMappingOrForwardPort, true};
    }
    return {ConnectionPreflightOutcome::DirectMayNeedHelp,
            ConnectionPreflightAction::CheckFirewall, true};
}

} // namespace jamlink::network
