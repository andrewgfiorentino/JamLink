// Copyright (c) 2026 Andrew Fiorentino
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
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using jamlink::network::IPeerAudioTransport;
using jamlink::network::PeerConnectionState;

jamlink::network::PeerParticipantInfo participant(
    std::string profileId,
    std::string displayName,
    std::string applicationVersion = "0.3.0",
    std::string buildIdentity = "test-build") {
    jamlink::network::PeerParticipantInfo result;
    result.profileId = std::move(profileId);
    result.handle = "musician";
    result.displayName = std::move(displayName);
    result.avatarId = "avatar:guitar-electric";
    result.primaryInstrument = "Guitar";
    result.applicationVersion = std::move(applicationVersion);
    result.buildIdentity = std::move(buildIdentity);
    result.releaseChannel = "test";
    return result;
}

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

// The port the host bound, whichever invite form it produced.
std::uint16_t invitePort(const std::string& invite) {
    const auto colon = invite.rfind(':');
    const auto pipe = invite.find('|', 4U);
    if (invite.rfind("JL2|", 0U) == 0U && colon != std::string::npos) {
        const auto end = invite.find_first_not_of("0123456789", colon + 1U);
        return static_cast<std::uint16_t>(std::stoul(invite.substr(
            colon + 1U, end == std::string::npos ? std::string::npos : end - colon - 1U)));
    }
    if (pipe == std::string::npos) {
        return 0U;
    }
    const auto portEnd = invite.find('|', pipe + 1U);
    if (portEnd == std::string::npos) {
        return 0U;
    }
    return static_cast<std::uint16_t>(
        std::stoul(invite.substr(pipe + 1U, portEnd - pipe - 1U)));
}

std::string rewriteInviteEndpoint(
    const std::string& invite,
    const std::string& address,
    std::uint16_t port) {
    const auto addressEnd = invite.find('|', 4U);
    if (addressEnd == std::string::npos) {
        return invite;
    }
    // A JL2 invite names every address the host can be reached on. Replacing
    // the whole list with one loopback candidate is what makes these tests a
    // test of the transport rather than of whatever network the machine is on.
    if (invite.rfind("JL2|", 0U) == 0U) {
        return "JL2|h=" + address + ":" + std::to_string(port) + invite.substr(addressEnd);
    }
    if (invite.rfind("JL1|", 0U) != 0U) {
        return invite;
    }
    const auto portEnd = invite.find('|', addressEnd + 1U);
    if (portEnd == std::string::npos) {
        return invite;
    }
    return "JL1|" + address + "|" + std::to_string(port) + invite.substr(portEnd);
}

std::string forceLoopback(const std::string& invite) {
    const std::uint16_t port = invitePort(invite);
    if (port == 0U) {
        return invite;
    }
    return rewriteInviteEndpoint(invite, "127.0.0.1", port);
}

// Candidates a router will never carry, ahead of the one that works. This is
// the shape of every real two-home attempt: a LAN address the other house
// cannot reach, and a public address whose router may or may not cooperate.
std::string withDeadCandidatesFirst(const std::string& invite) {
    const auto addressEnd = invite.find('|', 4U);
    const std::uint16_t port = invitePort(invite);
    if (invite.rfind("JL2|", 0U) != 0U || addressEnd == std::string::npos || port == 0U) {
        return invite;
    }
    // 192.0.2.0/24 is reserved for documentation and is routed nowhere.
    return "JL2|h=192.0.2.10:" + std::to_string(port)
        + ",s=192.0.2.11:9" + ",h=127.0.0.1:" + std::to_string(port)
        + invite.substr(addressEnd);
}

std::array<float, 128U> makeTone(double increment, double amplitude) {
    std::array<float, 128U> samples{};
    for (std::size_t index = 0U; index < samples.size(); ++index) {
        samples[index] = static_cast<float>(
            std::sin(static_cast<double>(index) * increment) * amplitude);
    }
    return samples;
}

struct StreamPeaks final {
    float hostInstrument{0.0F};
    float hostVoice{0.0F};
    float guestInstrument{0.0F};
    float guestVoice{0.0F};
};

float peakOf(std::span<const float> samples) {
    float peak = 0.0F;
    for (const float sample : samples) {
        if (std::isfinite(sample)) {
            peak = std::max(peak, std::abs(sample));
        }
    }
    return peak;
}

// Drives both peers against the wall clock, because the receive jitter buffer
// takes its playout clock from pullRemote48k. Pulling faster than real time
// would starve it permanently and pulling slower would overflow it, so a test
// that ignores pacing measures nothing useful.
//
// Instrument and voice carry different tones, so a transport that merged or
// crossed them would fail the separation checks.
StreamPeaks pump(
    IPeerAudioTransport& host,
    IPeerAudioTransport& guest,
    std::chrono::milliseconds duration,
    std::chrono::milliseconds measureLast) {
    using jamlink::network::AudioStreamId;
    using Clock = std::chrono::steady_clock;

    const auto hostInstrument = makeTone(0.07, 0.4);
    const auto hostVoice = makeTone(0.23, 0.35);
    const auto guestInstrument = makeTone(0.11, 0.3);
    const auto guestVoice = makeTone(0.31, 0.28);

    const auto start = Clock::now();
    const auto finish = start + duration;
    const auto measureFrom = finish - measureLast;
    std::size_t pushedFrames = 0U;
    std::size_t pulledFrames = 0U;
    StreamPeaks peaks;
    std::array<float, 240U> block{};

    while (Clock::now() < finish) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start).count();
        const auto dueFrames = static_cast<std::size_t>(
            std::max<std::int64_t>(elapsed, 0) * 48 / 1'000);

        while (pushedFrames + hostInstrument.size() <= dueFrames) {
            host.pushLocalAudio(AudioStreamId::Instrument, hostInstrument, 48'000U);
            host.pushLocalAudio(AudioStreamId::Voice, hostVoice, 48'000U);
            guest.pushLocalAudio(AudioStreamId::Instrument, guestInstrument, 48'000U);
            guest.pushLocalAudio(AudioStreamId::Voice, guestVoice, 48'000U);
            pushedFrames += hostInstrument.size();
        }

        const bool measuring = Clock::now() >= measureFrom;
        while (pulledFrames + block.size() <= dueFrames) {
            static_cast<void>(host.pullRemote48k(AudioStreamId::Instrument, block));
            if (measuring) {
                peaks.hostInstrument = std::max(peaks.hostInstrument, peakOf(block));
            }
            static_cast<void>(host.pullRemote48k(AudioStreamId::Voice, block));
            if (measuring) {
                peaks.hostVoice = std::max(peaks.hostVoice, peakOf(block));
            }
            static_cast<void>(guest.pullRemote48k(AudioStreamId::Instrument, block));
            if (measuring) {
                peaks.guestInstrument = std::max(peaks.guestInstrument, peakOf(block));
            }
            static_cast<void>(guest.pullRemote48k(AudioStreamId::Voice, block));
            if (measuring) {
                peaks.guestVoice = std::max(peaks.guestVoice, peakOf(block));
            }
            pulledFrames += block.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return peaks;
}

constexpr float audibleThreshold = 0.05F;
constexpr float silentThreshold = 0.01F;

bool exchangeAudio(IPeerAudioTransport& host, IPeerAudioTransport& guest) {
    const StreamPeaks peaks = pump(
        host, guest, std::chrono::milliseconds(900), std::chrono::milliseconds(400));
    return peaks.hostInstrument > audibleThreshold
        && peaks.hostVoice > audibleThreshold
        && peaks.guestInstrument > audibleThreshold
        && peaks.guestVoice > audibleThreshold
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
    check(transport && !transport->telemetry().udpBound
              && transport->localPort() == 0U,
          "invalid invite cleanup never reports a closed UDP socket as bound");
}

void reportsDeterministicOfflineHostPreflight() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    if (!host) {
        check(false, "offline preflight transport factory");
        return;
    }
    const std::string invite = host->host(0U, false);
    const auto telemetry = host->telemetry();
    check(!invite.empty(), "offline preflight creates the direct fallback invite");
    check(telemetry.udpBound, "offline preflight reports successful UDP bind");
    check(telemetry.publicAddressDiscovery
              == jamlink::network::PublicAddressDiscoveryState::NotAttempted,
          "offline preflight does not pretend public discovery ran");
    check(telemetry.portMapping == jamlink::network::PortMappingState::NotRequested,
          "offline preflight does not pretend router mapping ran");
    check(telemetry.reachability == jamlink::network::ReachabilityAssessment::Unknown,
          "offline preflight leaves Internet reachability unknown");
    check(!telemetry.automaticPortMapping,
          "offline preflight does not report automatic mapping");

    host->stop();
    const auto stopped = host->telemetry();
    check(!stopped.udpBound
              && stopped.publicAddressDiscovery
                  == jamlink::network::PublicAddressDiscoveryState::NotAttempted
              && stopped.portMapping == jamlink::network::PortMappingState::NotRequested,
          "stopping clears preflight telemetry before the next room");
}

// Zero is a legitimate "no measurement yet" for the round trip, and the UI
// labels this value "measured", so the transport has to say which it is.
// Per-stream accepted counts matter for the same reason: a loss rate divided by
// every datagram the transport ever received understates loss by about half.
void reportsRoundTripOnlyOnceMeasured() {
    using jamlink::network::AudioStreamId;
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "round trip harness setup");
        return;
    }
    const std::string invite = forceLoopback(host->host(0U, false));
    if (invite.empty()) {
        check(false, "round trip harness host start");
        return;
    }
    check(
        !host->telemetry().roundTripMeasured,
        "round trip is not claimed as measured before any pong");
    check(
        host->telemetry().roundTripMicroseconds == 0U,
        "unmeasured round trip stays at zero rather than a stale value");

    if (!guest->join(invite) || !waitForConnected(*host, *guest)
        || !exchangeAudio(*host, *guest)) {
        check(false, "round trip harness handshake");
        return;
    }

    const auto telemetry = host->telemetry();
    // The ping cadence is 500 ms and exchangeAudio runs longer than that.
    check(telemetry.roundTripMeasured, "round trip becomes measured once a pong returns");
    check(
        telemetry.streams[static_cast<std::size_t>(AudioStreamId::Instrument)].packetsAccepted > 0U
            && telemetry.streams[static_cast<std::size_t>(AudioStreamId::Voice)].packetsAccepted > 0U,
        "each stream publishes its own accepted packet count");
    // The per-stream counts must be a strict subset of the transport total,
    // which is what makes the transport-wide figure the wrong denominator.
    const std::uint64_t perStream =
        telemetry.streams[static_cast<std::size_t>(AudioStreamId::Instrument)].packetsAccepted
        + telemetry.streams[static_cast<std::size_t>(AudioStreamId::Voice)].packetsAccepted;
    check(
        perStream <= telemetry.packetsReceived,
        "stream accepted counts never exceed everything the transport received");

    host->stop();
    guest->stop();
}

void exchangesEncryptedAudioOnLoopback() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "platform peer transport factory");
        return;
    }
    const std::string hostInvite = host->host(0U, false);
    const std::string portField = ":" + std::to_string(host->localPort());
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

    // Stop reflecting before reading any counter. Sampling the telemetry while
    // the relay still runs races the reflection count forward and makes the
    // comparison below fail for timing reasons rather than security ones.
    stopRelay.store(true, std::memory_order_release);
    relayThread.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    const std::uint64_t reflectedCount = reflected.load(std::memory_order_relaxed);
    const auto hostTelemetry = host->telemetry();
    const auto guestTelemetry = guest->telemetry();

    check(reflectedCount > 0U, "reflection harness echoed traffic");

    // The security property, stated so that timing cannot satisfy it by
    // accident: the host may only ever accept packets the guest actually sent.
    // Accepting its own reflected traffic would push its received count above
    // everything the guest ever put on the wire.
    check(
        hostTelemetry.packetsReceived <= guestTelemetry.packetsSent,
        "host never accepts a reflected packet");
    // And they must be actively rejected rather than silently lost. Loopback
    // can still drop a datagram on a loaded runner, so this allows for some
    // shortfall while staying far above zero.
    check(
        hostTelemetry.packetsRejected * 2U >= reflectedCount,
        "host rejects its own reflected packets");
    check(audio, "session survives reflection attack");
    check(
        hostTelemetry.state == PeerConnectionState::Connected,
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

    std::atomic<bool> floodRunning{true};
    std::thread flood([&] {
        std::array<std::uint8_t, 1'400U> datagram{};
        for (std::size_t attempt = 0U; attempt < 20'000U
             && floodRunning.load(std::memory_order_acquire); ++attempt) {
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
    });

    const bool audioDuringFlood = exchangeAudio(*host, *guest);
    floodRunning.store(false, std::memory_order_release);
    flood.join();

    check(
        host->telemetry().state == PeerConnectionState::Connected,
        "session survives hostile datagrams");
    check(audioDuringFlood, "audio flows concurrently with hostile datagrams");
    check(guest->telemetry().state == PeerConnectionState::Connected,
        "peer remains connected during hostile datagrams");

    host->stop();
    guest->stop();
}

// A listener must be able to turn a friend's guitar down without touching
// their voice, which is only possible if the two never share a stream.
void remoteStreamsAreIndependentlyControllable() {
    using jamlink::network::AudioStreamId;
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "stream independence harness setup");
        return;
    }
    const std::string invite = forceLoopback(host->host(0U, false));
    if (invite.empty() || !guest->join(invite) || !waitForConnected(*host, *guest)
        || !exchangeAudio(*host, *guest)) {
        check(false, "stream independence harness handshake");
        return;
    }

    // Measure only the tail of each window so the previous setting's audio has
    // fully drained out of the receive buffer first.
    const auto settle = [&](bool wantInstrument, bool wantVoice) {
        const StreamPeaks peaks = pump(
            *host, *guest, std::chrono::milliseconds(900), std::chrono::milliseconds(300));
        const bool instrumentOk = wantInstrument
            ? peaks.hostInstrument > audibleThreshold
            : peaks.hostInstrument < silentThreshold;
        const bool voiceOk = wantVoice
            ? peaks.hostVoice > audibleThreshold
            : peaks.hostVoice < silentThreshold;
        if (!instrumentOk || !voiceOk) {
            std::cout << "    wanted instrument "
                      << (wantInstrument ? "audible" : "silent") << ", voice "
                      << (wantVoice ? "audible" : "silent")
                      << "; measured instrument " << peaks.hostInstrument
                      << ", voice " << peaks.hostVoice << "\n";
        }
        return instrumentOk && voiceOk;
    };

    host->setRemoteStreamMuted(AudioStreamId::Instrument, true);
    check(settle(false, true), "muting the remote instrument leaves voice audible");

    host->setRemoteStreamMuted(AudioStreamId::Instrument, false);
    host->setRemoteStreamMuted(AudioStreamId::Voice, true);
    check(settle(true, false), "muting the remote voice leaves the instrument audible");

    host->setRemoteStreamMuted(AudioStreamId::Voice, false);
    check(settle(true, true), "unmuting restores both remote streams");

    // Muting an outgoing stream must not silence the other direction's pair.
    guest->setLocalStreamMuted(AudioStreamId::Instrument, true);
    check(settle(false, true), "muting a sender's instrument leaves its voice flowing");

    host->stop();
    guest->stop();
}

// A muted stream emits no audio packets, so the per-packet clip flag's carrier
// is gone exactly when the state most needs reporting. Without this the
// receiver can only say nothing is arriving, which reads as a broken link and
// sends both musicians hunting a connection fault that does not exist.
void reportsADeliberateMuteAsAMuteRatherThanSilence() {
    using jamlink::network::AudioStreamId;
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "mute reporting harness setup");
        return;
    }
    const std::string invite = forceLoopback(host->host(0U, false));
    if (invite.empty() || !guest->join(invite) || !waitForConnected(*host, *guest)) {
        check(false, "mute reporting harness handshake");
        return;
    }
    // The state rides the control packet, which is sent twice a second.
    const auto settle = [&host, &guest]() {
        static_cast<void>(pump(
            *host, *guest, std::chrono::milliseconds(1'400),
            std::chrono::milliseconds(200)));
    };

    guest->setLocalStreamMuted(AudioStreamId::Instrument, true);
    settle();
    auto hostView = host->telemetry();
    check(hostView.streams[0].mutedByPeer && !hostView.streams[1].mutedByPeer,
          "a muted instrument is reported as muted while voice is not");

    guest->setLocalStreamMuted(AudioStreamId::Instrument, false);
    guest->setSendMuted(true);
    settle();
    hostView = host->telemetry();
    check(hostView.streams[0].mutedByPeer && hostView.streams[1].mutedByPeer,
          "a room-wide mute is reported on both streams");

    guest->setSendMuted(false);
    settle();
    hostView = host->telemetry();
    check(!hostView.streams[0].mutedByPeer && !hostView.streams[1].mutedByPeer,
          "unmuting clears the reported mute");
    // The control packet grew by a byte, and the reply still carries only the
    // timestamp, so the round trip must be unaffected.
    check(hostView.roundTripMeasured, "the round trip survives the larger control packet");

    host->stop();
    guest->stop();
}

// Proves Opus actually crosses the wire, rather than the encoder having failed
// to initialise and the stream having quietly fallen back to uncompressed. The
// fallback is deliberate and silent by design, which is exactly why it needs a
// test that would notice.
void carriesOpusAudioOverTheWire() {
    using jamlink::network::AudioStreamId;
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "opus transport harness setup");
        return;
    }
    const std::string invite = forceLoopback(host->host(0U, false));
    if (invite.empty() || !guest->join(invite) || !waitForConnected(*host, *guest)) {
        check(false, "opus transport harness handshake");
        return;
    }

    const auto peaks = pump(
        *host, *guest, std::chrono::milliseconds(900), std::chrono::milliseconds(300));
    const auto hostView = host->telemetry();

    check(hostView.opusPacketsDecoded > 0U,
          "audio arrives Opus-encoded rather than falling back to uncompressed");
    check(hostView.undecodablePackets == 0U,
          "no packet carried a codec tag this build could not use");
    check(hostView.encodeFailures == 0U, "the encoder refused nothing");
    // And it is still audible after the round trip, which is the point.
    check(peaks.hostInstrument > audibleThreshold,
          "Opus audio is audible at the far end");
    std::cout << "    opus packets " << hostView.opusPacketsDecoded
              << ", uncompressed " << hostView.pcmPacketsDecoded
              << ", bitrate " << hostView.audioBitsPerSecond << "\n";

    host->stop();
    guest->stop();
}

void carriesAuthenticatedSourceClipStatusIndependently() {
    using jamlink::network::AudioStreamId;
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "source clip status harness setup");
        return;
    }
    const std::string invite = forceLoopback(host->host(0U, false));
    if (invite.empty() || !guest->join(invite) || !waitForConnected(*host, *guest)) {
        check(false, "source clip status harness handshake");
        return;
    }

    host->setLocalStreamClipState(AudioStreamId::Instrument, true);
    guest->setLocalStreamClipState(AudioStreamId::Voice, true);
    static_cast<void>(pump(
        *host, *guest, std::chrono::milliseconds(500), std::chrono::milliseconds(200)));
    const auto guestView = guest->telemetry();
    const auto hostView = host->telemetry();
    check(guestView.streams[0].sourceClipped && !guestView.streams[1].sourceClipped,
          "remote instrument clip report stays independent of voice");
    check(!hostView.streams[0].sourceClipped && hostView.streams[1].sourceClipped,
          "remote voice clip report stays independent of instrument");

    host->setLocalStreamClipState(AudioStreamId::Instrument, false);
    guest->setLocalStreamClipState(AudioStreamId::Voice, false);
    static_cast<void>(pump(
        *host, *guest, std::chrono::milliseconds(500), std::chrono::milliseconds(200)));
    check(!guest->telemetry().streams[0].sourceClipped
              && !host->telemetry().streams[1].sourceClipped,
          "authenticated source clip reports clear after the sender resets them");

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
    // Play for a while before leaving. A handshake-only first session sends so
    // few packets that the host's replay window still accepts the rejoining
    // guest's restarted counter by accident, which hid a real reconnect bug.
    if (!exchangeAudio(*host, *guest)) {
        check(false, "reconnect harness first session audio");
        return;
    }
    const std::uint64_t firstSessionPackets = guest->telemetry().packetsSent;
    check(firstSessionPackets > 64U, "first session outlives the replay window");
    guest->stop();

    // JL1 has no cryptographic device identity or resume token. Fail closed:
    // the host must first expire the old endpoint before a fresh endpoint can
    // reuse the bearer invite. JL2 will replace this delay with signed,
    // rotating session resumption.
    const auto expiryDeadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(6);
    while (std::chrono::steady_clock::now() < expiryDeadline
           && host->telemetry().state != PeerConnectionState::WaitingForPeer) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(host->telemetry().state == PeerConnectionState::WaitingForPeer,
          "host expires the old endpoint before JL1 reconnect");

    // Rejoining reuses the room secret after that fail-closed expiry, so the
    // derived keys and nonce prefixes are re-established from a clean session.
    const bool rejoined = guest->join(invite) && waitForConnected(*host, *guest);
    check(rejoined, "guest rejoins the same room after leaving");
    check(rejoined && exchangeAudio(*host, *guest), "audio resumes after rejoin");

    host->stop();
    guest->stop();
}

void negotiatesExactBuildAndExchangesReliableChat() {
    using jamlink::network::RoomControlEventType;
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "room control harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew"));
    guest->setLocalParticipant(participant("profile-guest", "Mike"));
    const std::string invite = forceLoopback(host->host(0U, false));
    const bool connected = !invite.empty() && guest->join(invite)
        && waitForConnected(*host, *guest);
    check(connected, "authenticated build negotiation accepts the exact build");
    if (!connected) {
        return;
    }
    check(host->remoteParticipant().displayName == "Mike"
              && guest->remoteParticipant().displayName == "Andrew",
          "authenticated participant identity reaches both peers");
    static_cast<void>(host->takeControlEvents());
    static_cast<void>(guest->takeControlEvents());

    const std::string message = "Sounds good \xF0\x9F\x8E\xB8";
    check(host->sendChatMessage(message), "valid UTF-8 chat enters bounded send queue");
    bool received = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !received) {
        for (const auto& event : guest->takeControlEvents()) {
            if (event.type == RoomControlEventType::ChatMessage
                && event.text == message && event.participant.displayName == "Andrew") {
                received = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(received, "encrypted reliable chat carries text and sender identity");

    const std::string invalidUtf8{"\xC0\xAF", 2U};
    check(!host->sendChatMessage(invalidUtf8), "invalid UTF-8 chat is rejected");
    check(!host->sendChatMessage(std::string(
              jamlink::network::maximumChatMessageBytes + 1U, 'x')),
          "oversized chat is rejected");

    host->stop();
    guest->stop();
}

void rejectsIncompatibleApplicationBuild() {
    using jamlink::network::RoomControlEventType;
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "version mismatch harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew", "0.3.0", "build-a"));
    guest->setLocalParticipant(participant("profile-guest", "Mike", "0.3.0", "build-b"));
    const std::string invite = forceLoopback(host->host(0U, false));
    check(!invite.empty() && guest->join(invite), "version mismatch handshake starts");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline
           && guest->telemetry().state != PeerConnectionState::VersionMismatch) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    check(host->telemetry().state == PeerConnectionState::VersionMismatch
              && guest->telemetry().state == PeerConnectionState::VersionMismatch,
          "authenticated handshake rejects a different build identity");
    bool mismatchEvent = false;
    for (const auto& event : guest->takeControlEvents()) {
        mismatchEvent = mismatchEvent || event.type == RoomControlEventType::VersionMismatch;
    }
    check(mismatchEvent, "version mismatch produces a user-facing control event");
    check(host->telemetry().packetsReceived > 0U
              && guest->telemetry().packetsReceived > 0U,
          "version rejection is an authenticated exchange");
    host->stop();
    guest->stop();
}

void additionalGuestCannotDisplaceActivePerformer() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    auto additionalGuest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest || !additionalGuest) {
        check(false, "additional-guest isolation harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew"));
    guest->setLocalParticipant(participant("profile-guest", "Mike"));
    // A copied invite plus a copied, self-asserted profile ID must not be
    // mistaken for a secure reconnect while the real performer is healthy.
    additionalGuest->setLocalParticipant(participant("profile-guest", "Impostor"));
    const std::string invite = forceLoopback(host->host(0U, false));
    const bool connected = !invite.empty() && guest->join(invite)
        && waitForConnected(*host, *guest);
    check(connected, "first performer connects before an additional join");
    if (!connected) {
        return;
    }

    check(additionalGuest->join(invite), "additional performer can attempt the room invite");
    std::this_thread::sleep_for(std::chrono::milliseconds(750));
    check(host->telemetry().state == PeerConnectionState::Connected
              && guest->telemetry().state == PeerConnectionState::Connected,
          "additional join never displaces the active performer");
    check(host->remoteParticipant().profileId == "profile-guest",
          "host keeps the original authenticated participant identity");
    check(host->remoteParticipant().displayName == "Mike",
          "copied profile ID cannot repin the active endpoint");
    check(exchangeAudio(*host, *guest),
          "active two-person audio continues during an additional join attempt");

    additionalGuest->stop();
    host->stop();
    guest->stop();
}

// One address in an invite is a guess. Field testing kept producing the same
// failure: a public address discovered, an invite that looked correct, and no
// connection -- because the only address named was the one the routers would
// not carry. Naming every address and probing all of them is the fix.
void connectsThroughACandidateThatIsNotTheFirstOffered() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "candidate probing harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew"));
    guest->setLocalParticipant(participant("profile-guest", "Mike"));
    const std::string offered = host->host(0U, false);
    check(offered.rfind("JL2|", 0U) == 0U, "the host offers a candidate list");

    const std::string invite = withDeadCandidatesFirst(offered);
    const bool connected = guest->join(invite) && waitForConnected(*host, *guest);
    // Two unroutable candidates come first. Anything that stops at the first
    // address, or that treats a packet leaving as proof the path works, never
    // gets here.
    check(connected, "an unreachable first candidate does not prevent connection");
    if (connected) {
        check(exchangeAudio(*host, *guest),
              "audio flows on the candidate that actually answered");
    }
    host->stop();
    guest->stop();
}

void keepsProbingAfterAnEarlyRoundFindsNothing() {
    // A router that refuses now can cooperate later, and a musician is still
    // sitting there. Exhausting the candidates must start another round rather
    // than leaving a dead session that looks identical to a working one.
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!guest) {
        check(false, "probe persistence harness setup");
        return;
    }
    guest->setLocalParticipant(participant("profile-guest", "Mike"));
    const bool joined = guest->join(
        "JL2|h=192.0.2.10:41234,s=192.0.2.11:41234|"
        "0000000000000000000000000000000000000000000000000000000000000000");
    check(joined, "a join to unreachable candidates still starts");
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    const auto telemetry = guest->telemetry();
    check(telemetry.state != PeerConnectionState::Connected,
          "nothing answered, so nothing is reported as connected");
    check(telemetry.packetsSent > 0U,
          "probes are actually sent to candidates that will never answer");
    guest->stop();
}

// In a mesh everybody has to be findable by everybody else, and only the host
// publishes addresses today because only the host has to be found. The room's
// creator is the one participant guaranteed to know everyone, so guests tell it
// where they are and it has something real to introduce people with.
void aGuestTellsTheRoomWhereItCanBeReached() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto guest = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !guest) {
        check(false, "roster harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew"));
    guest->setLocalParticipant(participant("profile-guest", "Mike"));
    check(host->roomMembers().empty(), "a room nobody has reported into is empty");

    const std::string invite = forceLoopback(host->host(0U, false));
    const bool connected = !invite.empty() && guest->join(invite)
        && waitForConnected(*host, *guest);
    check(connected, "roster harness connects");
    if (!connected) {
        return;
    }

    // Published once the session is up rather than during the handshake, so it
    // cannot delay a connection forming.
    std::vector<jamlink::network::RosterMember> members;
    for (int attempt = 0; attempt < 200 && members.empty(); ++attempt) {
        members = host->roomMembers();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(members.size() == 1U, "the guest reaches the room's roster");
    if (members.size() == 1U) {
        check(members[0].participantId == "profile-guest",
              "the roster names the authenticated participant, not a claimed one");
        check(!members[0].candidates.empty(),
              "the roster carries somewhere the guest can actually be reached");
    }

    // Both ends report, because both run the same worker -- and that is right
    // rather than incidental. In a mesh everybody has to be findable by
    // everybody, and a guest that already knows the host's addresses can
    // re-form a dropped session without being handed a fresh invite.
    const auto asKnownToGuest = guest->roomMembers();
    check(asKnownToGuest.size() == 1U
              && asKnownToGuest[0].participantId == "profile-host",
          "the host is findable by the guest as well, not only the reverse");

    check(exchangeAudio(*host, *guest), "audio still flows alongside the report");
    host->stop();
    guest->stop();
}

// Drives a whole room against the wall clock, and returns what each musician
// heard.
//
// Not two pumps side by side. The send path captures, limits and encodes once
// and seals the result separately for each recipient, so the way it fails is
// one musician hearing everything and everybody after them hearing silence.
// Only a pump that drives the whole room at once can see that.
std::vector<float> pumpRoom(
    const std::vector<IPeerAudioTransport*>& room,
    std::chrono::milliseconds duration,
    std::chrono::milliseconds measureLast) {
    using jamlink::network::AudioStreamId;
    using Clock = std::chrono::steady_clock;

    // A tone each, so a transport that crossed two musicians' audio would not
    // be able to pass by accident.
    std::vector<std::array<float, 128U>> tones;
    tones.reserve(room.size());
    for (std::size_t index = 0U; index < room.size(); ++index) {
        tones.push_back(makeTone(0.07 + 0.05 * static_cast<double>(index), 0.4));
    }
    std::vector<float> peaks(room.size(), 0.0F);

    const auto start = Clock::now();
    const auto finish = start + duration;
    const auto measureFrom = finish - measureLast;
    std::size_t pushedFrames = 0U;
    std::size_t pulledFrames = 0U;
    std::array<float, 240U> block{};

    while (Clock::now() < finish) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            Clock::now() - start).count();
        const auto dueFrames = static_cast<std::size_t>(
            std::max<std::int64_t>(elapsed, 0) * 48 / 1'000);
        while (pushedFrames + 128U <= dueFrames) {
            for (std::size_t index = 0U; index < room.size(); ++index) {
                room[index]->pushLocalAudio(
                    AudioStreamId::Instrument, tones[index], 48'000U);
                room[index]->pushLocalAudio(AudioStreamId::Voice, tones[index], 48'000U);
            }
            pushedFrames += 128U;
        }
        const bool measuring = Clock::now() >= measureFrom;
        while (pulledFrames + block.size() <= dueFrames) {
            for (std::size_t index = 0U; index < room.size(); ++index) {
                static_cast<void>(
                    room[index]->pullRemote48k(AudioStreamId::Instrument, block));
                if (measuring) {
                    peaks[index] = std::max(peaks[index], peakOf(block));
                }
                static_cast<void>(room[index]->pullRemote48k(AudioStreamId::Voice, block));
            }
            pulledFrames += block.size();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return peaks;
}

bool waitForPeerCount(
    IPeerAudioTransport& transport,
    std::uint32_t peers,
    std::chrono::milliseconds patience) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        if (transport.telemetry().connectedPeers == peers) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool waitForState(
    IPeerAudioTransport& transport,
    PeerConnectionState state,
    std::chrono::milliseconds patience) {
    const auto deadline = std::chrono::steady_clock::now() + patience;
    while (std::chrono::steady_clock::now() < deadline) {
        if (transport.telemetry().state == state) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// A third musician arrives at a room that is already playing, and everybody
// ends up hearing everybody they should.
//
// This is the test the two-person suite could not be: with one peer, servicing
// one slot and servicing every slot are the same code path, and so are
// draining the send pacer once and draining it per recipient. Both only come
// apart when somebody else is in the room.
void admitsASecondMusicianAndSendsToBoth() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto first = jamlink::network::createPlatformPeerAudioTransport();
    auto second = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !first || !second) {
        check(false, "three-musician harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew"));
    first->setLocalParticipant(participant("profile-first", "Mike"));
    second->setLocalParticipant(participant("profile-second", "Sam"));

    const std::string invite = forceLoopback(host->host(0U, false));
    const bool firstConnected = !invite.empty() && first->join(invite)
        && waitForConnected(*host, *first);
    check(firstConnected, "the first musician connects");
    if (!firstConnected) {
        return;
    }
    const std::uint64_t firstReceivedBefore = first->telemetry().packetsReceived;

    // Mid-session, which is the case that matters. A join request from an
    // endpoint nothing is expecting has to find a slot of its own rather than
    // being rejected as not-the-peer or handed the established one.
    check(second->join(invite), "a second musician can attempt the room");
    check(waitForPeerCount(*host, 2U, std::chrono::milliseconds(6'000)),
          "the host gives the arriving musician a slot of their own");
    check(waitForConnected(*host, *second), "the second musician connects");
    check(first->telemetry().state == PeerConnectionState::Connected,
          "the arrival never disturbs the session already running");

    const auto peaks = pumpRoom(
        {host.get(), first.get(), second.get()},
        std::chrono::milliseconds(1'500), std::chrono::milliseconds(600));
    check(peaks[0] > audibleThreshold, "the host hears the room");
    check(peaks[1] > audibleThreshold, "the first musician hears the host");
    // THE ONE THAT MATTERS, and the reason this test exists. The send pacer
    // releases one packet on a schedule. Draining it once per recipient hands
    // the first musician every packet and everybody after them silence, and it
    // is invisible in every two-person test there is.
    check(peaks[2] > audibleThreshold,
          "the second musician hears the host too, so the pacer is not drained per peer");

    const auto hostView = host->telemetry();
    check(hostView.peers[0].connected && hostView.peers[1].connected,
          "the host reports two separate links rather than one");
    check(first->telemetry().packetsReceived > firstReceivedBefore,
          "the first musician kept receiving across the second one arriving");
    // Each link is measured on its own. One number for the room would be a
    // different musician's connection every time it moved.
    check(hostView.peers[0].roundTripMeasured && hostView.peers[1].roundTripMeasured,
          "each link is timed separately");

    second->stop();
    first->stop();
    host->stop();
}

// One musician leaves. The rest keep playing, and the room says so accurately.
//
// A single connected flag could not describe this, and a single receive
// deadline would have timed the whole room out on the silence of whoever left.
// It is also where "connected" and "complete" visibly stop being the same
// question: two people can still play, and the room is no longer whole.
void oneMusicianLeavingLeavesTheRestPlaying() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    auto staying = jamlink::network::createPlatformPeerAudioTransport();
    auto leaving = jamlink::network::createPlatformPeerAudioTransport();
    if (!host || !staying || !leaving) {
        check(false, "departure harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew"));
    staying->setLocalParticipant(participant("profile-staying", "Mike"));
    leaving->setLocalParticipant(participant("profile-leaving", "Sam"));

    const std::string invite = forceLoopback(host->host(0U, false));
    const bool ready = !invite.empty() && staying->join(invite) && leaving->join(invite)
        && waitForPeerCount(*host, 2U, std::chrono::milliseconds(6'000));
    check(ready, "a room of three forms");
    if (!ready) {
        return;
    }
    // Both guests report where they can be reached once their session is up,
    // so the room knows how many people it is expecting.
    const auto whole = [&host] {
        for (int attempt = 0; attempt < 300; ++attempt) {
            if (host->telemetry().roomComplete) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }();
    check(whole, "a room with everybody in it reports itself whole");

    leaving->stop();
    // Past the receive deadline for the musician who left, while the room
    // keeps playing. A shared deadline would take everybody down here.
    const auto peaks = pumpRoom(
        {host.get(), staying.get()},
        std::chrono::milliseconds(7'000), std::chrono::milliseconds(800));

    const auto hostView = host->telemetry();
    check(hostView.state == PeerConnectionState::Connected,
          "a room somebody left is still a room the rest can play in");
    check(hostView.connectedPeers == 1U, "only the musician who left is reported gone");
    // The whole point of naming the two questions separately: still connected,
    // no longer whole. One flag cannot say both.
    check(!hostView.roomComplete,
          "the room is no longer whole, which is a different answer to still being connected");
    check(peaks[0] > audibleThreshold && peaks[1] > audibleThreshold,
          "audio keeps flowing between the musicians who stayed");
    check(staying->telemetry().state == PeerConnectionState::Connected,
          "the musician who stayed was never disconnected");

    staying->stop();
    host->stop();
}

// A room that cannot carry another musician says so.
//
// Silence would be indistinguishable from a network that never carried them:
// somebody watching a spinner has no way to tell "the room is full" from
// "JamLink is broken", and the second is what they would conclude.
void aRefusedMusicianIsToldRatherThanLeftWaiting() {
    auto host = jamlink::network::createPlatformPeerAudioTransport();
    if (!host) {
        check(false, "capacity harness setup");
        return;
    }
    host->setLocalParticipant(participant("profile-host", "Andrew"));
    const std::string invite = forceLoopback(host->host(0U, false));
    if (invite.empty()) {
        check(false, "capacity harness host start");
        return;
    }

    // As many as a direct session is built for. Past this the number of paths
    // grows faster than anybody's connection does, which is the capacity
    // guard's rule rather than a second one invented at the door.
    std::vector<std::unique_ptr<IPeerAudioTransport>> admitted;
    bool allJoined = true;
    for (std::size_t index = 0U; index < jamlink::network::maximumRoomPeers; ++index) {
        auto guest = jamlink::network::createPlatformPeerAudioTransport();
        if (!guest) {
            allJoined = false;
            break;
        }
        guest->setLocalParticipant(participant(
            "profile-guest-" + std::to_string(index), "Musician " + std::to_string(index)));
        allJoined = allJoined && guest->join(invite);
        admitted.push_back(std::move(guest));
    }
    check(allJoined, "a full room of musicians attempts to join");
    const bool full = waitForPeerCount(
        *host, static_cast<std::uint32_t>(jamlink::network::maximumRoomPeers),
        std::chrono::milliseconds(15'000));
    check(full, "the room fills to what a mesh is built for");
    if (!full) {
        for (auto& guest : admitted) {
            guest->stop();
        }
        host->stop();
        return;
    }

    auto refused = jamlink::network::createPlatformPeerAudioTransport();
    if (!refused) {
        check(false, "refused musician harness setup");
        return;
    }
    refused->setLocalParticipant(participant("profile-refused", "Late"));
    check(refused->join(invite), "one more musician attempts the room");
    check(waitForState(*refused, PeerConnectionState::RoomFull,
                       std::chrono::milliseconds(6'000)),
          "the refused musician is told the room is full rather than left waiting");

    bool toldInWords = false;
    for (const auto& event : refused->takeControlEvents()) {
        toldInWords = toldInWords
            || event.type == jamlink::network::RoomControlEventType::RoomFull;
    }
    check(toldInWords, "the refusal reaches the interface as something to show somebody");
    check(host->telemetry().connectedPeers
              == static_cast<std::uint32_t>(jamlink::network::maximumRoomPeers),
          "a refused musician never displaces one who is already in the room");

    refused->stop();
    for (auto& guest : admitted) {
        guest->stop();
    }
    host->stop();
}

} // namespace

int main() {
    WSADATA winsock{};
    if (WSAStartup(MAKEWORD(2, 2), &winsock) != 0) {
        std::cerr << "[FAIL] winsock startup\n";
        return 1;
    }

    rejectsMalformedInvites();
    aGuestTellsTheRoomWhereItCanBeReached();
    connectsThroughACandidateThatIsNotTheFirstOffered();
    keepsProbingAfterAnEarlyRoundFindsNothing();
    reportsDeterministicOfflineHostPreflight();
    reportsRoundTripOnlyOnceMeasured();
    exchangesEncryptedAudioOnLoopback();
    rejectsReflectedOwnTraffic();
    survivesHostileDatagrams();
    remoteStreamsAreIndependentlyControllable();
    carriesAuthenticatedSourceClipStatusIndependently();
    carriesOpusAudioOverTheWire();
    reportsADeliberateMuteAsAMuteRatherThanSilence();
    reconnectsAfterGuestRestart();
    negotiatesExactBuildAndExchangesReliableChat();
    rejectsIncompatibleApplicationBuild();
    additionalGuestCannotDisplaceActivePerformer();
    admitsASecondMusicianAndSendsToBoth();
    oneMusicianLeavingLeavesTheRestPlaying();
    aRefusedMusicianIsToldRatherThanLeftWaiting();

    static_cast<void>(WSACleanup());
    std::cout << (failures == 0U ? "all peer transport checks passed\n"
                                 : "peer transport checks failed\n");
    return failures == 0U ? 0 : 1;
}
