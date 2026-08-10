// SPDX-License-Identifier: GPL-3.0-or-later

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include "jamlink/network/peer_audio_transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace {

using jamlink::network::IPeerAudioTransport;
using jamlink::network::PeerConnectionState;

bool waitForConnected(IPeerAudioTransport& first, IPeerAudioTransport& second) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (first.telemetry().state == PeerConnectionState::Connected
            && second.telemetry().state == PeerConnectionState::Connected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

std::string rewriteInviteEndpoint(
    const std::string& invite,
    const std::string& address,
    std::uint16_t port) {
    const auto addressEnd = invite.find('|', 4U);
    if (invite.rfind("JL1|", 0U) != 0U || addressEnd == std::string::npos) {
        return invite;
    }
    const auto portEnd = invite.find('|', addressEnd + 1U);
    if (portEnd == std::string::npos) {
        return invite;
    }
    return "JL1|" + address + "|" + std::to_string(port) + invite.substr(portEnd);
}

std::string forceLoopback(const std::string& invite) {
    const auto addressEnd = invite.find('|', 4U);
    const auto portEnd = addressEnd == std::string::npos
        ? std::string::npos : invite.find('|', addressEnd + 1U);
    if (portEnd == std::string::npos) {
        return invite;
    }
    const std::string port = invite.substr(addressEnd + 1U, portEnd - addressEnd - 1U);
    return rewriteInviteEndpoint(
        invite, "127.0.0.1", static_cast<std::uint16_t>(std::stoul(port)));
}

std::array<float, 128U> makeTone(double increment, double amplitude) {
    std::array<float, 128U> samples{};
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        samples[index] = static_cast<float>(
            std::sin(static_cast<double>(index) * increment) * amplitude);
    }
    return samples;
}

bool hasSignal(std::span<const float> samples) {
    return std::any_of(samples.begin(), samples.end(), [](float sample) {
        return std::isfinite(sample) && std::abs(sample) > 0.01F;
    });
}

// Pumps audio in both directions until both peers hold a full network packet.
bool exchangeAudio(IPeerAudioTransport& host, IPeerAudioTransport& guest) {
    const auto hostTone = makeTone(0.07, 0.4);
    const auto guestTone = makeTone(0.11, 0.3);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline
           && (host.availableRemote48k() < 240U || guest.availableRemote48k() < 240U)) {
        host.pushLocalAudio(hostTone, 48'000U);
        guest.pushLocalAudio(guestTone, 48'000U);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    std::array<float, 240U> atHost{};
    std::array<float, 240U> atGuest{};
    if (host.availableRemote48k() < atHost.size()
        || guest.availableRemote48k() < atGuest.size()) {
        return false;
    }
    return host.pullRemote48k(atHost) == atHost.size()
        && guest.pullRemote48k(atGuest) == atGuest.size()
        && hasSignal(atHost) && hasSignal(atGuest)
        && host.telemetry().packetsReceived > 0U
        && guest.telemetry().packetsReceived > 0U;
}

class UdpSocket final {
public:
    UdpSocket() : value_(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) {}
    ~UdpSocket() {
        if (value_ != INVALID_SOCKET) {
            static_cast<void>(closesocket(value_));
        }
    }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_SOCKET; }
    [[nodiscard]] SOCKET get() const noexcept { return value_; }

    [[nodiscard]] bool bindLoopback() noexcept {
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        local.sin_port = 0U;
        if (bind(value_, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            return false;
        }
        int size = sizeof(local);
        if (getsockname(value_, reinterpret_cast<sockaddr*>(&local), &size) != 0) {
            return false;
        }
        port_ = ntohs(local.sin_port);
        return true;
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    SOCKET value_{INVALID_SOCKET};
    std::uint16_t port_{0U};
};

sockaddr_in loopbackEndpoint(std::uint16_t port) noexcept {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    return address;
}

bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) noexcept {
    return left.sin_port == right.sin_port && left.sin_addr.s_addr == right.sin_addr.s_addr;
}

std::size_t failures = 0U;

void check(bool condition, const char* description) {
    if (condition) {
        std::cout << "[PASS] " << description << '\n';
    } else {
        ++failures;
        std::cout << "[FAIL] " << description << '\n';
    }
}

void rejectsMalformedInvites() {
    auto transport = jamlink::network::createPlatformPeerAudioTransport();
    const bool rejected = transport
        && !transport->join("JL1|127.0.0.1|1234|not-a-secret")
        && transport->telemetry().state == PeerConnectionState::InviteInvalid
        && !transport->join("")
        && !transport->join("JL1|127.0.0.1|0|"
                            "0000000000000000000000000000000000000000000000000000000000000000")
        && !transport->join("JL2|127.0.0.1|1234|"
                            "0000000000000000000000000000000000000000000000000000000000000000")
        && !transport->join("JL1|999.999.999.999|1234|"
                            "0000000000000000000000000000000000000000000000000000000000000000");
    check(rejected, "malformed invites are rejected");
}

void exchangesEncryptedAudioOnLoopback() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "platform peer transport factory");
        return;
    }
    const std::string hostInvite = host->host(0U, false);
    const std::string portField = "|" + std::to_string(host->localPort()) + "|";
    const bool started = !hostInvite.empty()
        && hostInvite.find(portField) != std::string::npos
        && guest->join(forceLoopback(hostInvite))
        && waitForConnected(*host, *guest);
    check(started, "encrypted loopback invite handshake");
    check(started && exchangeAudio(*host, *guest), "encrypted two-peer loopback audio");
    host->stop();
    guest->stop();
}

// Relays guest and host traffic so the peers stay connected, while also
// echoing each host datagram straight back to the host from the pinned
// endpoint. A single shared key would authenticate that reflected audio.
void rejectsReflectedOwnTraffic() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    UdpSocket relay;
    if (!host || !guest || !relay.valid() || !relay.bindLoopback()) {
        check(false, "reflection harness setup");
        return;
    }

    const std::string hostInvite = host->host(0U, false);
    if (hostInvite.empty()) {
        check(false, "reflection harness host start");
        return;
    }
    const sockaddr_in hostEndpoint = loopbackEndpoint(host->localPort());

    std::atomic<bool> stopRelay{false};
    std::atomic<std::uint64_t> reflected{0U};
    std::thread relayThread([&] {
        sockaddr_in guestEndpoint{};
        bool haveGuest = false;
        std::array<std::uint8_t, 2'048U> buffer{};
        while (!stopRelay.load(std::memory_order_acquire)) {
            fd_set readSet;
            FD_ZERO(&readSet);
            FD_SET(relay.get(), &readSet);
            timeval timeout{0, 5'000};
            if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0) {
                continue;
            }
            sockaddr_in source{};
            int sourceSize = sizeof(source);
            const int bytes = recvfrom(
                relay.get(), reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()), 0,
                reinterpret_cast<sockaddr*>(&source), &sourceSize);
            if (bytes <= 0) {
                continue;
            }
            const bool fromHost = sameEndpoint(source, hostEndpoint);
            if (!fromHost) {
                guestEndpoint = source;
                haveGuest = true;
            }
            const sockaddr_in destination = fromHost ? guestEndpoint : hostEndpoint;
            if (fromHost && !haveGuest) {
                continue;
            }
            static_cast<void>(sendto(
                relay.get(), reinterpret_cast<const char*>(buffer.data()), bytes, 0,
                reinterpret_cast<const sockaddr*>(&destination), sizeof(destination)));
            if (fromHost) {
                // The attack: hand the host back its own authenticated packet
                // from the endpoint it has already pinned.
                static_cast<void>(sendto(
                    relay.get(), reinterpret_cast<const char*>(buffer.data()), bytes, 0,
                    reinterpret_cast<const sockaddr*>(&hostEndpoint), sizeof(hostEndpoint)));
                reflected.fetch_add(1U, std::memory_order_relaxed);
            }
        }
    });

    const std::string relayInvite = rewriteInviteEndpoint(hostInvite, "127.0.0.1", relay.port());
    const bool connected = guest->join(relayInvite) && waitForConnected(*host, *guest);
    check(connected, "relayed handshake completes");

    const bool audio = connected && exchangeAudio(*host, *guest);
    const auto hostTelemetry = host->telemetry();

    stopRelay.store(true, std::memory_order_release);
    relayThread.join();

    check(reflected.load(std::memory_order_relaxed) > 0U, "reflection harness echoed traffic");
    // Every reflected datagram must fail authentication under the peer's own
    // receive key, so the rejection count has to cover them.
    check(
        hostTelemetry.packetsRejected >= reflected.load(std::memory_order_relaxed),
        "host rejects its own reflected packets");
    check(audio, "session survives reflection attack");
    check(
        host->telemetry().state == PeerConnectionState::Connected,
        "reflection does not tear down the session");

    host->stop();
    guest->stop();
}

// Floods an established session with garbage, truncated, and near-valid
// datagrams from an unrelated socket.
void survivesHostileDatagrams() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    UdpSocket attacker;
    if (!host || !guest || !attacker.valid() || !attacker.bindLoopback()) {
        check(false, "hostile datagram harness setup");
        return;
    }

    const std::string hostInvite = host->host(0U, false);
    if (hostInvite.empty() || !guest->join(forceLoopback(hostInvite))
        || !waitForConnected(*host, *guest)) {
        check(false, "hostile datagram harness handshake");
        return;
    }

    const sockaddr_in target = loopbackEndpoint(host->localPort());
    std::uint64_t state = 0x243F6A8885A308D3ULL;
    const auto nextByte = [&state]() {
        state = state * 6'364'136'223'846'793'005ULL + 1'442'695'040'888'963'407ULL;
        return static_cast<std::uint8_t>(state >> 56U);
    };

    std::array<std::uint8_t, 1'400U> datagram{};
    for (std::size_t attempt = 0U; attempt < 2'000U; ++attempt) {
        for (std::uint8_t& byte : datagram) {
            byte = nextByte();
        }
        // A quarter of the attempts wear a valid magic and version so the
        // parser is pushed past its cheapest rejection.
        if (attempt % 4U == 0U) {
            datagram[0] = 0x4AU;
            datagram[1] = 0x4CU;
            datagram[2] = 0x4BU;
            datagram[3] = 0x31U;
            datagram[4] = 2U;
        }
        const int length = static_cast<int>(1U + (attempt * 7U) % datagram.size());
        static_cast<void>(sendto(
            attacker.get(), reinterpret_cast<const char*>(datagram.data()), length, 0,
            reinterpret_cast<const sockaddr*>(&target), sizeof(target)));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    check(
        host->telemetry().state == PeerConnectionState::Connected,
        "session survives hostile datagrams");
    check(exchangeAudio(*host, *guest), "audio still flows after hostile datagrams");

    host->stop();
    guest->stop();
}

void reconnectsAfterGuestRestart() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "reconnect harness setup");
        return;
    }
    const std::string invite = forceLoopback(host->host(0U, false));
    if (invite.empty() || !guest->join(invite) || !waitForConnected(*host, *guest)) {
        check(false, "reconnect harness handshake");
        return;
    }
    guest->stop();

    // Rejoining reuses the room secret, so the derived keys and nonce prefixes
    // must be re-established rather than continuing an old counter.
    const bool rejoined = guest->join(invite) && waitForConnected(*host, *guest);
    check(rejoined, "guest rejoins the same room after leaving");
    check(rejoined && exchangeAudio(*host, *guest), "audio resumes after rejoin");

    host->stop();
    guest->stop();
}

} // namespace

int main() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::cerr << "[FAIL] winsock startup\n";
        return 1;
    }

    rejectsMalformedInvites();
    exchangesEncryptedAudioOnLoopback();
    rejectsReflectedOwnTraffic();
    survivesHostileDatagrams();
    reconnectsAfterGuestRestart();

    static_cast<void>(WSACleanup());
    std::cout << (failures == 0U ? "all peer transport checks passed\n"
                                 : "peer transport checks failed\n");
    return failures == 0U ? 0 : 1;
}
