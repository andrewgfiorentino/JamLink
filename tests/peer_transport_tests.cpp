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

    // Rejoining reuses the room secret, so the derived keys and nonce prefixes
    // must be re-established rather than continuing an old counter.
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
    additionalGuest->setLocalParticipant(participant("profile-extra", "Chris"));
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
    check(exchangeAudio(*host, *guest),
          "active two-person audio continues during an additional join attempt");

    additionalGuest->stop();
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
    remoteStreamsAreIndependentlyControllable();
    carriesAuthenticatedSourceClipStatusIndependently();
    reconnectsAfterGuestRestart();
    negotiatesExactBuildAndExchangesReliableChat();
    rejectsIncompatibleApplicationBuild();
    additionalGuestCannotDisplaceActivePerformer();

    static_cast<void>(WSACleanup());
    std::cout << (failures == 0U ? "all peer transport checks passed\n"
                                 : "peer transport checks failed\n");
    return failures == 0U ? 0 : 1;
}
