// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// Reports what this network actually allows, so a failed jam can be diagnosed
// from facts instead of guesses. Run it on both machines and compare.
//
// It creates a real host session exactly as the application would, then prints
// the result of each step. No audio is captured and nothing is transmitted to
// anyone: the only outbound traffic is the STUN query and the router mapping
// request the application already performs.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <netfw.h>
#include <wrl/client.h>

#include "jamlink/diagnostics/session_log.hpp"
#include "jamlink/network/peer_audio_transport.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

const char* describe(jamlink::network::PortMappingState state) {
    using State = jamlink::network::PortMappingState;
    switch (state) {
    case State::NotRequested:
        return "not requested";
    case State::Succeeded:
        return "succeeded";
    case State::Failed:
        return "failed";
    }
    return "unknown";
}

const char* describe(jamlink::network::PublicAddressDiscoveryState state) {
    using State = jamlink::network::PublicAddressDiscoveryState;
    switch (state) {
    case State::NotAttempted:
        return "not attempted";
    case State::Succeeded:
        return "succeeded";
    case State::Failed:
        return "failed";
    }
    return "unknown";
}

const char* describe(jamlink::network::ReachabilityAssessment assessment) {
    using Assessment = jamlink::network::ReachabilityAssessment;
    switch (assessment) {
    case Assessment::Unknown:
        return "unknown";
    case Assessment::LikelyReachable:
        return "likely reachable";
    case Assessment::LikelyBlocked:
        return "likely blocked by the router or firewall";
    case Assessment::RelayRequired:
        return "relay required; a direct connection cannot work here";
    }
    return "unknown";
}

// A router mapping only gets the packet as far as the PC. Windows Firewall
// then decides whether anything receives it, and for an unsigned application
// run out of a ZIP the answer is usually no unless the user accepted the
// prompt on the right network profile. This is invisible from inside JamLink:
// every step reports success and the audio simply never arrives.
struct FirewallStatus final {
    bool queried{false};
    bool domainOn{false};
    bool privateOn{false};
    bool publicOn{false};
    long currentProfiles{0};
};

[[nodiscard]] FirewallStatus queryFirewall() {
    FirewallStatus status;
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialised) && initialised != RPC_E_CHANGED_MODE) {
        return status;
    }
    {
        Microsoft::WRL::ComPtr<INetFwPolicy2> policy;
        if (SUCCEEDED(CoCreateInstance(
                __uuidof(NetFwPolicy2), nullptr, CLSCTX_INPROC_SERVER,
                IID_PPV_ARGS(&policy)))
            && policy) {
            VARIANT_BOOL enabled = VARIANT_FALSE;
            if (SUCCEEDED(policy->get_FirewallEnabled(NET_FW_PROFILE2_DOMAIN, &enabled))) {
                status.domainOn = enabled != VARIANT_FALSE;
            }
            if (SUCCEEDED(policy->get_FirewallEnabled(NET_FW_PROFILE2_PRIVATE, &enabled))) {
                status.privateOn = enabled != VARIANT_FALSE;
            }
            if (SUCCEEDED(policy->get_FirewallEnabled(NET_FW_PROFILE2_PUBLIC, &enabled))) {
                status.publicOn = enabled != VARIANT_FALSE;
            }
            static_cast<void>(policy->get_CurrentProfileTypes(&status.currentProfiles));
            status.queried = true;
        }
    }
    if (SUCCEEDED(initialised)) {
        CoUninitialize();
    }
    return status;
}

[[nodiscard]] std::string activeProfileName(long profiles) {
    if ((profiles & NET_FW_PROFILE2_PUBLIC) != 0) {
        return "Public";
    }
    if ((profiles & NET_FW_PROFILE2_PRIVATE) != 0) {
        return "Private";
    }
    if ((profiles & NET_FW_PROFILE2_DOMAIN) != 0) {
        return "Domain";
    }
    return "unknown";
}

// The invite carries the room secret, so it must never be printed by a
// diagnostic the user is likely to paste into a chat window.
std::string redactInvite(const std::string& invite) {
    const auto lastSeparator = invite.rfind('|');
    if (lastSeparator == std::string::npos) {
        return invite.empty() ? "(none)" : "(malformed)";
    }
    return invite.substr(0U, lastSeparator + 1U) + "<secret withheld>";
}

} // namespace

int main() {
    // Same log the application writes, so a probe run and a failed jam land in
    // one file the user can send.
    char* localAppData = nullptr;
    std::size_t localAppDataSize = 0U;
    if (_dupenv_s(&localAppData, &localAppDataSize, "LOCALAPPDATA") == 0
        && localAppData != nullptr) {
        jamlink::diagnostics::SessionLog::instance().open(
            std::filesystem::path(localAppData) / "JamLink");
        std::free(localAppData);
    }

    auto transport = jamlink::network::createPlatformPeerAudioTransport();
    if (!transport) {
        std::cerr << "This build has no peer transport.\n";
        return 2;
    }

    std::cout << "JamLink network probe\n"
              << "---------------------\n"
              << "Opening a UDP port and asking the router and a public STUN\n"
              << "server what the outside world can see.\n\n";

    const std::string invite = transport->host();
    const auto telemetry = transport->telemetry();

    std::cout << "UDP port bound        : "
              << (telemetry.udpBound ? "yes" : "no") << '\n'
              << "Local UDP port        : " << transport->localPort() << '\n'
              << "Router port mapping   : " << describe(telemetry.portMapping);
    const std::string protocol = telemetry.portMappingProtocol == nullptr
        ? std::string{} : std::string(telemetry.portMappingProtocol);
    if (!protocol.empty()) {
        std::cout << " (via " << protocol << ')';
    }
    std::cout << '\n'
              << "Public address lookup : "
              << describe(telemetry.publicAddressDiscovery) << '\n'
              << "Reachability          : " << describe(telemetry.reachability) << '\n'
              << "Invite                : " << redactInvite(invite) << "\n\n";

    const FirewallStatus firewall = queryFirewall();
    if (firewall.queried) {
        const std::string profile = activeProfileName(firewall.currentProfiles);
        const bool activeProfileOn =
            (profile == "Public" && firewall.publicOn)
            || (profile == "Private" && firewall.privateOn)
            || (profile == "Domain" && firewall.domainOn);
        std::cout << "Active network profile : " << profile << '\n'
                  << "Windows Firewall      : "
                  << (activeProfileOn ? "on for this profile" : "off for this profile")
                  << "\n\n";
        if (activeProfileOn) {
            std::cout << "The firewall is on. A router mapping only delivers the packet\n"
                      << "to this PC; the firewall then decides whether JamLink receives\n"
                      << "it. If you never saw an allow prompt, or answered it for a\n"
                      << "different profile than " << profile << ", inbound audio is being\n"
                      << "dropped here and every other check will still look healthy.\n\n"
                      << "To allow it explicitly, in an Administrator PowerShell:\n"
                      << "  New-NetFirewallRule -DisplayName 'JamLink' -Direction Inbound"
                      << " -Program '<full path>\\JamLink.exe' -Protocol UDP -Action Allow\n\n";
        }
    }

    if (telemetry.portMapping == jamlink::network::PortMappingState::Succeeded) {
        std::cout << "Router mapping is in place, so the remaining thing that can\n"
                  << "silently drop inbound audio on this machine is the firewall above.\n";
    } else {
        std::cout << "No router mapping was granted, through UPnP, PCP, or NAT-PMP.\n"
                  << "Hosting from here will usually fail: your router discards the\n"
                  << "incoming audio before JamLink can see it. Options, best first:\n"
                  << "  1. Enable UPnP, PCP, or NAT-PMP in the router settings.\n"
                  << "  2. Forward UDP port " << transport->localPort()
                  << " to this PC, then host again.\n"
                  << "  3. Let the other person host instead, if their probe passes.\n"
                  << "  4. If neither side can map a port, a direct connection is not\n"
                  << "     possible on these networks and a relay is required.\n";
    }

    const auto logPath = jamlink::diagnostics::SessionLog::instance().path();
    if (!logPath.empty()) {
        std::cout << "\nDetail written to: " << logPath.string() << '\n';
    }

    // Leave the mapping in place briefly so a router UI can be inspected.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    transport->stop();
    return telemetry.portMapping == jamlink::network::PortMappingState::Succeeded ? 0 : 1;
}
