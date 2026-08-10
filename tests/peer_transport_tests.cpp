// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/peer_audio_transport.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <thread>

namespace {

bool waitForConnected(
    jamlink::network::IPeerAudioTransport& first,
    jamlink::network::IPeerAudioTransport& second) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (first.telemetry().state == jamlink::network::PeerConnectionState::Connected
            && second.telemetry().state == jamlink::network::PeerConnectionState::Connected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::string forceLoopback(std::string invite) {
    const auto addressEnd = invite.find('|', 4U);
    if (invite.rfind("JL1|", 0U) == 0U && addressEnd != std::string::npos) {
        invite.replace(4U, addressEnd - 4U, "127.0.0.1");
    }
    return invite;
}

} // namespace

int main() {
    auto invalid = jamlink::network::createPlatformPeerAudioTransport();
    if (!invalid || invalid->join("JL1|127.0.0.1|1234|not-a-secret")
        || invalid->telemetry().state
            != jamlink::network::PeerConnectionState::InviteInvalid) {
        std::cerr << "[FAIL] malformed invite rejection\n";
        return 1;
    }

    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        std::cerr << "[FAIL] platform peer transport factory\n";
        return 1;
    }
    const std::string hostInvite = host->host(0U, false);
    const std::string portField = "|" + std::to_string(host->localPort()) + "|";
    const std::string invite = forceLoopback(hostInvite);
    if (hostInvite.empty() || hostInvite.find(portField) == std::string::npos
        || !guest->join(invite) || !waitForConnected(*host, *guest)) {
        std::cerr << "[FAIL] encrypted loopback invite handshake\n";
        return 1;
    }

    std::array<float, 128U> hostAudio{};
    std::array<float, 128U> guestAudio{};
    for (std::size_t index = 0U; index < hostAudio.size(); ++index) {
        hostAudio[index] = static_cast<float>(std::sin(index * 0.07) * 0.4);
        guestAudio[index] = static_cast<float>(std::sin(index * 0.11) * 0.3);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline
           && (host->availableRemote48k() < 240U || guest->availableRemote48k() < 240U)) {
        host->pushLocalAudio(hostAudio, 48'000U);
        guest->pushLocalAudio(guestAudio, 48'000U);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::array<float, 240U> atHost{};
    std::array<float, 240U> atGuest{};
    const auto hostFrames = host->availableRemote48k() >= atHost.size()
        ? host->pullRemote48k(atHost) : 0U;
    const auto guestFrames = guest->availableRemote48k() >= atGuest.size()
        ? guest->pullRemote48k(atGuest) : 0U;
    const auto hasSignal = [](const auto& samples) {
        return std::any_of(samples.begin(), samples.end(), [](float sample) {
            return std::isfinite(sample) && std::abs(sample) > 0.01F;
        });
    };
    const bool passed = hostFrames == atHost.size() && guestFrames == atGuest.size()
        && hasSignal(atHost) && hasSignal(atGuest)
        && host->telemetry().packetsReceived > 0U
        && guest->telemetry().packetsReceived > 0U;
    host->stop();
    guest->stop();
    std::cout << (passed
        ? "[PASS] encrypted two-peer loopback audio\n"
        : "[FAIL] encrypted two-peer loopback audio\n");
    return passed ? 0 : 1;
}
