// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace jamlink::network {

enum class PeerConnectionState : std::uint8_t {
    Idle,
    Preparing,
    WaitingForPeer,
    Connecting,
    Connected,
    InviteInvalid,
    SocketFailed,
    EncryptionFailed,
    ConnectionLost
};

struct PeerTransportTelemetry final {
    PeerConnectionState state{PeerConnectionState::Idle};
    std::uint64_t packetsSent{0U};
    std::uint64_t packetsReceived{0U};
    std::uint64_t packetsRejected{0U};
    std::uint64_t localAudioDrops{0U};
    std::uint64_t remoteAudioDrops{0U};
    float remotePeak{0.0F};
    std::uint32_t roundTripMilliseconds{0U};
    bool automaticPortMapping{false};
};

// Realtime side of the transport. These calls only touch bounded lock-free
// audio queues; sockets and cryptography are owned by the network worker.
class IPeerAudioExchange {
public:
    virtual ~IPeerAudioExchange() = default;
    virtual void pushLocalAudio(
        std::span<const float> monoSamples,
        std::uint32_t sampleRate) noexcept = 0;
    [[nodiscard]] virtual std::size_t pullRemote48k(
        std::span<float> monoSamples) noexcept = 0;
    [[nodiscard]] virtual std::size_t availableRemote48k() const noexcept = 0;
};

class IPeerAudioTransport : public IPeerAudioExchange {
public:
    ~IPeerAudioTransport() override = default;

    // A zero port selects an available UDP port. The returned invite is empty
    // on failure and contains the address, port, and a random 256-bit secret.
    [[nodiscard]] virtual std::string host(
        std::uint16_t port = 0U,
        bool prepareInternetReachability = true) = 0;
    [[nodiscard]] virtual bool join(const std::string& inviteCode) = 0;
    virtual void stop() noexcept = 0;
    virtual void setSendMuted(bool muted) noexcept = 0;
    [[nodiscard]] virtual std::string inviteCode() const = 0;
    [[nodiscard]] virtual std::uint16_t localPort() const noexcept = 0;
    [[nodiscard]] virtual PeerTransportTelemetry telemetry() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IPeerAudioTransport> createPlatformPeerAudioTransport();

} // namespace jamlink::network
