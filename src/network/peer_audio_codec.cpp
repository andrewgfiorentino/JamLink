// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/peer_audio_codec.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace jamlink::network {
namespace {

[[nodiscard]] std::size_t pcmBytes(std::size_t frameSamples) noexcept {
    return frameSamples * 2U;
}

void decodePcm16(std::span<const std::uint8_t> data, std::span<float> frame) noexcept {
    for (std::size_t index = 0U; index < frame.size(); ++index) {
        const auto sample = static_cast<std::int16_t>(
            static_cast<std::uint16_t>(data[index * 2U])
            | (static_cast<std::uint16_t>(data[index * 2U + 1U]) << 8U));
        frame[index] = static_cast<float>(sample) / 32'768.0F;
    }
}

void encodePcm16(std::span<const float> frame, std::span<std::uint8_t> data) noexcept {
    for (std::size_t index = 0U; index < frame.size(); ++index) {
        const float bounded = std::clamp(frame[index], -0.98F, 0.98F);
        const auto sample = static_cast<std::int16_t>(std::lrint(bounded * 32'767.0F));
        data[index * 2U] = static_cast<std::uint8_t>(sample & 0xFF);
        data[index * 2U + 1U] = static_cast<std::uint8_t>((sample >> 8) & 0xFF);
    }
}

} // namespace

JamLinkStreamDecoder::JamLinkStreamDecoder(
    std::uint32_t sampleRate,
    std::size_t frameSamples)
    : frameSamples_(frameSamples), opus_(sampleRate, frameSamples) {}

std::size_t JamLinkStreamDecoder::decode(
    std::span<const std::uint8_t> packet,
    std::span<float> frame) noexcept {
    if (frame.size() != frameSamples_ || packet.size() < codecTagBytes) {
        ++unusable_;
        return 0U;
    }
    const std::uint8_t tag = packet[0];
    const auto body = packet.subspan(codecTagBytes);

    if (tag == static_cast<std::uint8_t>(PeerAudioCodec::Pcm16)) {
        if (body.size() != pcmBytes(frameSamples_)) {
            ++unusable_;
            return 0U;
        }
        decodePcm16(body, frame);
        ++pcmDecoded_;
        return frameSamples_;
    }

    if (tag == static_cast<std::uint8_t>(PeerAudioCodec::Opus)) {
        const std::size_t produced = opus_.decode(body, frame);
        if (produced != frameSamples_) {
            ++unusable_;
            return 0U;
        }
        opusCarriedAudio_ = true;
        ++opusDecoded_;
        return frameSamples_;
    }

    // A tag this build does not know. Counted rather than guessed at: decoding
    // it as something else would put arbitrary noise into the room.
    ++unusable_;
    return 0U;
}

std::size_t JamLinkStreamDecoder::conceal(std::span<float> frame) noexcept {
    if (frame.size() != frameSamples_) {
        return 0U;
    }
    if (!opusCarriedAudio_) {
        // Nothing stateful is carrying this stream, so there is no decoder
        // state to keep in step and no codec-specific concealment worth having.
        // The receiver's pitch-synchronous extrapolation covers the gap.
        return 0U;
    }
    return opus_.conceal(frame);
}

void JamLinkStreamDecoder::reset() noexcept {
    opus_.reset();
    opusCarriedAudio_ = false;
}

std::size_t JamLinkStreamDecoder::maximumPacketBytes() const noexcept {
    // Whichever wire format is larger has to fit, because either may arrive.
    return codecTagBytes
        + std::max(pcmBytes(frameSamples_), audio::OpusStreamEncoder::maximumPacketBytes());
}

JamLinkStreamEncoder::JamLinkStreamEncoder(
    std::uint32_t sampleRate,
    std::size_t frameSamples,
    std::uint32_t bitsPerSecond,
    audio::OpusStreamEncoder::Content content)
    : frameSamples_(frameSamples),
      opus_(sampleRate, frameSamples, bitsPerSecond, content) {}

void JamLinkStreamEncoder::setCodec(PeerAudioCodec codec) noexcept {
    // Asking for Opus on a machine where the encoder could not be created would
    // otherwise mean silence, which is the least useful failure available.
    codec_ = (codec == PeerAudioCodec::Opus && !opus_.valid())
        ? PeerAudioCodec::Pcm16 : codec;
}

bool JamLinkStreamEncoder::setBitsPerSecond(std::uint32_t bitsPerSecond) noexcept {
    return opus_.setBitsPerSecond(bitsPerSecond);
}

std::uint32_t JamLinkStreamEncoder::bitsPerSecond() const noexcept {
    return opus_.bitsPerSecond();
}

std::size_t JamLinkStreamEncoder::maximumPacketBytes(std::size_t frameSamples) noexcept {
    return codecTagBytes
        + std::max(pcmBytes(frameSamples), audio::OpusStreamEncoder::maximumPacketBytes());
}

std::size_t JamLinkStreamEncoder::encode(
    std::span<const float> frame,
    std::span<std::uint8_t> packet) noexcept {
    if (frame.size() != frameSamples_ || packet.size() < codecTagBytes + 1U) {
        return 0U;
    }
    if (codec_ == PeerAudioCodec::Opus) {
        const std::size_t written =
            opus_.encode(frame, packet.subspan(codecTagBytes));
        if (written != 0U) {
            packet[0] = static_cast<std::uint8_t>(PeerAudioCodec::Opus);
            return codecTagBytes + written;
        }
        // One refused frame does not justify silence. Fall through and send it
        // uncompressed, which the receiver reads from the tag without needing
        // to be told anything.
    }
    if (packet.size() < codecTagBytes + pcmBytes(frameSamples_)) {
        return 0U;
    }
    packet[0] = static_cast<std::uint8_t>(PeerAudioCodec::Pcm16);
    encodePcm16(frame, packet.subspan(codecTagBytes));
    return codecTagBytes + pcmBytes(frameSamples_);
}

} // namespace jamlink::network
