// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
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

// Independent logical streams. Instrument audio and voice travel separately so
// a listener can turn one down without the other, and so speech-oriented
// processing can never be applied to an instrument. The transport carries the
// identifier on the wire, so more streams can be added without a redesign.
enum class AudioStreamId : std::uint8_t {
    Instrument = 0U,
    Voice = 1U
};

inline constexpr std::size_t audioStreamCount = 2U;

[[nodiscard]] constexpr std::size_t streamIndex(AudioStreamId stream) noexcept {
    return static_cast<std::size_t>(stream);
}

struct RemoteStreamTelemetry final {
    float peak{0.0F};
    std::uint64_t packetsConcealed{0U};
    std::uint64_t packetsLate{0U};
    std::uint64_t bufferStretches{0U};
    std::uint64_t latencyTrims{0U};
    // Measured arrival jitter, not an estimate of end-to-end delay.
    std::uint32_t jitterMicroseconds{0U};
    std::uint32_t bufferedFrames{0U};
    std::uint32_t targetFrames{0U};
    bool playing{false};
};

struct PeerTransportTelemetry final {
    PeerConnectionState state{PeerConnectionState::Idle};
    std::uint64_t packetsSent{0U};
    std::uint64_t packetsReceived{0U};
    std::uint64_t packetsRejected{0U};
    std::uint64_t localAudioDrops{0U};
    // Measured round trip. Reported in microseconds because the useful range
    // for interactive playing is well below millisecond display resolution.
    std::uint64_t roundTripMicroseconds{0U};
    bool automaticPortMapping{false};
    std::array<RemoteStreamTelemetry, audioStreamCount> streams{};
};

// Realtime side of the transport. These calls only touch bounded lock-free
// audio queues; sockets and cryptography are owned by the network worker.
class IPeerAudioExchange {
public:
    virtual ~IPeerAudioExchange() = default;
    virtual void pushLocalAudio(
        AudioStreamId stream,
        std::span<const float> monoSamples,
        std::uint32_t sampleRate) noexcept = 0;
    // Always fills the destination, concealing gaps and inserting silence
    // before playout begins. Returns the frames carrying live remote audio.
    [[nodiscard]] virtual std::size_t pullRemote48k(
        AudioStreamId stream,
        std::span<float> monoSamples) noexcept = 0;
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

    // Mutes everything this peer sends, which is the room mute control.
    virtual void setSendMuted(bool muted) noexcept = 0;
    // Mutes one outgoing stream, so an instrument can be silenced while voice
    // keeps flowing.
    virtual void setLocalStreamMuted(AudioStreamId stream, bool muted) noexcept = 0;
    // How loudly this peer hears one of the remote streams.
    virtual void setRemoteStreamGain(AudioStreamId stream, float gain) noexcept = 0;
    virtual void setRemoteStreamMuted(AudioStreamId stream, bool muted) noexcept = 0;

    [[nodiscard]] virtual std::string inviteCode() const = 0;
    [[nodiscard]] virtual std::uint16_t localPort() const noexcept = 0;
    [[nodiscard]] virtual PeerTransportTelemetry telemetry() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IPeerAudioTransport> createPlatformPeerAudioTransport();

} // namespace jamlink::network
