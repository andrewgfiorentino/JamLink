// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/network/connection_preflight.hpp"
#include "jamlink/network/nat_behaviour.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace jamlink::network {

enum class PeerConnectionState : std::uint8_t {
    Idle,
    Preparing,
    WaitingForPeer,
    Connecting,
    Connected,
    InviteInvalid,
    VersionMismatch,
    SocketFailed,
    EncryptionFailed,
    ConnectionLost
};

// 3 frames every audio payload with the codec that produced it, so a
// packet is read by what it says it is rather than by prior agreement.
inline constexpr std::uint16_t currentMediaProtocolVersion = 3U;
// 2 adds the sender's per-stream mute state to the periodic control packet.
inline constexpr std::uint16_t currentControlProtocolVersion = 2U;
inline constexpr std::size_t maximumChatMessageBytes = 512U;

// Identity and compatibility information exchanged inside the authenticated
// room handshake. The visible handle is never used as the stable identity.
// These values live on the control/network worker only, never in an audio
// callback.
struct PeerParticipantInfo final {
    std::string profileId;
    std::string handle;
    std::string displayName;
    std::string avatarId;
    std::string primaryInstrument;
    std::string applicationVersion;
    std::string buildIdentity;
    std::string releaseChannel;
    std::uint16_t mediaProtocolVersion{currentMediaProtocolVersion};
    std::uint16_t controlProtocolVersion{currentControlProtocolVersion};

    bool operator==(const PeerParticipantInfo&) const = default;
};

enum class RoomControlEventType : std::uint8_t {
    PeerJoined,
    PeerLeft,
    ChatMessage,
    ChatDeliveryFailed,
    VersionMismatch
};

struct RoomControlEvent final {
    RoomControlEventType type{RoomControlEventType::ChatMessage};
    std::uint64_t messageId{0U};
    std::uint64_t timestampMilliseconds{0U};
    PeerParticipantInfo participant;
    std::string text;
};

// Independent logical streams. Instrument audio and voice travel separately so
// a listener can turn one down without the other, and so speech-oriented
// processing can never be applied to an instrument. The transport carries the
// identifier on the wire, so more streams can be added without a redesign.
// How much receive buffering the user is willing to trade for tolerance of a
// rough network. Lowest is for a good local path; MostStable survives more
// jitter at the cost of latency that eventually stops being playable.
enum class LatencyPreference : std::uint8_t {
    Lowest = 0U,
    Balanced = 1U,
    MostStable = 2U
};

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
    // Authenticated status reported by the sender's own source detector. This
    // is not inferred from received PCM and therefore does not overclaim which
    // physical stage clipped on the remote machine.
    bool sourceClipped{false};
    // The sender says it is deliberately not transmitting this stream. A muted
    // stream produces no packets at all, so without being told, the receiver
    // can only report that nothing is arriving -- which reads as a fault. This
    // arrives on the periodic control packet precisely because it must keep
    // working when no audio is being sent.
    bool mutedByPeer{false};
    // Audio packets accepted for this stream alone. A loss rate has to be
    // measured against the stream it belongs to; dividing by every datagram the
    // transport ever received folds in the other stream and the control
    // traffic, which understates loss by roughly half.
    std::uint64_t packetsAccepted{0U};
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
    // False until a Pong has actually come back. Zero is a legitimate value for
    // "no measurement yet", so it must never be presented as a measured one.
    bool roundTripMeasured{false};
    // Which of UPnP, PCP, or NAT-PMP opened the port, or empty if none did.
    // Points at a string literal.
    const char* portMappingProtocol{""};
    bool automaticPortMapping{false};
    bool udpBound{false};
    PublicAddressDiscoveryState publicAddressDiscovery{
        PublicAddressDiscoveryState::NotAttempted};
    PortMappingState portMapping{PortMappingState::NotRequested};
    ReachabilityAssessment reachability{ReachabilityAssessment::Unknown};
    std::array<RemoteStreamTelemetry, audioStreamCount> streams{};

    // Codec health. Diagnostics only: the musician is told whether the
    // connection is coping, not which codec is carrying it.
    //
    // Counted rather than inferred from a negotiated setting, because a packet
    // names its own format and an encoder that could not be created falls back
    // silently by design. These say what actually crossed the wire.
    std::uint64_t opusPacketsDecoded{0U};
    std::uint64_t pcmPacketsDecoded{0U};
    // Packets whose codec tag this build could not use. Never guessed at.
    std::uint64_t undecodablePackets{0U};
    // Frames the encoder refused, which fall back to uncompressed rather than
    // becoming silence.
    std::uint64_t encodeFailures{0U};
    std::uint32_t audioBitsPerSecond{0U};

    // How many times a peer session has actually been established on this
    // transport. The first is the join; every one after it is a reconnect.
    // Counted rather than inferred, because "it kept dropping out" is the
    // commonest report there is and nothing in a support bundle could
    // previously confirm or contradict it.
    std::uint64_t sessionsEstablished{0U};

    // Samples the send limiter had to hold below full scale on their way out.
    // Diagnostic only, and never a substitute for the clip latch: the fix for
    // a hot input is the gain knob, and a limiter that quietly cleaned it up
    // would remove the reason to go and turn it down.
    std::uint64_t limitedSendSamples{0U};

    // Path finding. A guest probes every address the invite named until one
    // answers. Both figures matter in a failure report: probes prove packets
    // actually left, and a round that exhausted every candidate without an
    // answer is the signature of two routers that will not carry the path.
    // What this machine's router does to it on the way out, which decides
    // whether an invite made here leads anywhere at all.
    NatMappingBehaviour natBehaviour{NatMappingBehaviour::NotProbed};
    std::uint64_t candidateProbesSent{0U};
    std::uint32_t candidateRoundsExhausted{0U};
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
    // Realtime-safe status update; the Windows transport implements this as a
    // lock-free atomic carried in the authenticated audio header.
    virtual void setLocalStreamClipState(AudioStreamId, bool) noexcept {}
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
        bool discoverPublicAddress = true,
        bool requestAutomaticPortMapping = true) = 0;
    [[nodiscard]] virtual bool join(const std::string& inviteCode) = 0;
    virtual void stop() noexcept = 0;

    // Must be configured while stopped. The application/build identity is
    // authenticated by the same AES-GCM session as audio before a peer can be
    // considered connected.
    virtual void setLocalParticipant(PeerParticipantInfo participant) = 0;

    // Reliable, bounded room control. Chat uses acknowledgements, retry and
    // deduplication independently of the non-retransmitted music packets.
    [[nodiscard]] virtual bool sendChatMessage(const std::string& plainText) = 0;
    [[nodiscard]] virtual std::vector<RoomControlEvent> takeControlEvents() = 0;
    [[nodiscard]] virtual PeerParticipantInfo remoteParticipant() const = 0;

    // Mutes everything this peer sends, which is the room mute control.
    virtual void setSendMuted(bool muted) noexcept = 0;
    // Mutes one outgoing stream, so an instrument can be silenced while voice
    // keeps flowing.
    virtual void setLocalStreamMuted(AudioStreamId stream, bool muted) noexcept = 0;
    // How loudly this peer hears one of the remote streams.
    virtual void setRemoteStreamGain(AudioStreamId stream, float gain) noexcept = 0;
    virtual void setRemoteStreamMuted(AudioStreamId stream, bool muted) noexcept = 0;
    // Applies while stopped; the next room uses it.
    virtual void setLatencyPreference(LatencyPreference preference) noexcept = 0;

    [[nodiscard]] virtual std::string inviteCode() const = 0;
    [[nodiscard]] virtual std::uint16_t localPort() const noexcept = 0;
    [[nodiscard]] virtual PeerTransportTelemetry telemetry() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<IPeerAudioTransport> createPlatformPeerAudioTransport();

} // namespace jamlink::network
