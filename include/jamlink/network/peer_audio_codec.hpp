// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/opus_stream_codec.hpp"
#include "jamlink/network/audio_packet_decoder.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace jamlink::network {

enum class PeerAudioCodec : std::uint8_t {
    // Uncompressed 16-bit samples. Kept as a first-class path rather than a
    // historical accident: it is the reference the codec is measured against,
    // the fallback if an encoder cannot be created on a machine, and what the
    // impairment tests run so they measure the jitter buffer rather than Opus.
    Pcm16 = 0U,
    Opus = 1U,
};

// Every audio payload names its own codec in its first byte.
//
// The alternative is negotiating once and remembering, which introduces a state
// both ends have to agree about and a window during which they can disagree. A
// packet that says what it is cannot be misread, survives a peer changing codec
// mid-session, and costs one byte against a payload of sixty.
inline constexpr std::size_t codecTagBytes = 1U;

[[nodiscard]] inline bool isSupportedCodec(std::uint8_t tag) noexcept {
    return tag == static_cast<std::uint8_t>(PeerAudioCodec::Pcm16)
        || tag == static_cast<std::uint8_t>(PeerAudioCodec::Opus);
}

// Decodes either wire format, dispatching on the tag each packet carries.
//
// Holds one Opus decoder for the life of the stream. That decoder is stateful
// and its state is only meaningful in sequence order, which is exactly why the
// receiver decodes at playout rather than on arrival.
class JamLinkStreamDecoder final : public IAudioPacketDecoder {
public:
    JamLinkStreamDecoder(std::uint32_t sampleRate, std::size_t frameSamples);

    [[nodiscard]] std::size_t decode(
        std::span<const std::uint8_t> packet,
        std::span<float> frame) noexcept override;

    [[nodiscard]] std::size_t conceal(std::span<float> frame) noexcept override;

    void reset() noexcept override;

    [[nodiscard]] std::size_t maximumPacketBytes() const noexcept override;

    // Diagnostics only. Never read from the audio callback's decisions.
    [[nodiscard]] std::uint64_t opusPacketsDecoded() const noexcept { return opusDecoded_; }
    [[nodiscard]] std::uint64_t pcmPacketsDecoded() const noexcept { return pcmDecoded_; }
    [[nodiscard]] std::uint64_t unusablePackets() const noexcept { return unusable_; }

private:
    std::size_t frameSamples_;
    audio::OpusStreamDecoder opus_;
    // True once an Opus packet has been decoded. Opus concealment is only
    // meaningful for an Opus stream; asking it to conceal a gap in a PCM stream
    // would synthesise from state that never described this audio.
    bool opusCarriedAudio_{false};
    std::uint64_t opusDecoded_{0U};
    std::uint64_t pcmDecoded_{0U};
    std::uint64_t unusable_{0U};
};

// Encodes one outgoing stream in the configured format, tag included.
class JamLinkStreamEncoder final {
public:
    JamLinkStreamEncoder(
        std::uint32_t sampleRate,
        std::size_t frameSamples,
        std::uint32_t bitsPerSecond,
        audio::OpusStreamEncoder::Content content);

    // Writes the tag and the encoded frame into `packet`. Returns total bytes,
    // or zero if the frame could not be encoded.
    [[nodiscard]] std::size_t encode(
        std::span<const float> frame,
        std::span<std::uint8_t> packet) noexcept;

    // Falls back to uncompressed if the encoder could not be created, so a
    // machine where Opus fails to initialise still plays rather than going
    // silent for a reason no musician could act on.
    void setCodec(PeerAudioCodec codec) noexcept;
    [[nodiscard]] PeerAudioCodec codec() const noexcept { return codec_; }
    [[nodiscard]] bool setBitsPerSecond(std::uint32_t bitsPerSecond) noexcept;
    [[nodiscard]] std::uint32_t bitsPerSecond() const noexcept;

    [[nodiscard]] static std::size_t maximumPacketBytes(std::size_t frameSamples) noexcept;

private:
    std::size_t frameSamples_;
    audio::OpusStreamEncoder opus_;
    PeerAudioCodec codec_{PeerAudioCodec::Pcm16};
};

} // namespace jamlink::network
