// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/opus_stream_codec.hpp"

#include <opus.h>

#include <algorithm>
#include <utility>

namespace jamlink::audio {
namespace {

[[nodiscard]] bool supportedFrame(std::uint32_t sampleRate, std::size_t frameSamples) noexcept {
    if (sampleRate == 0U || frameSamples == 0U) {
        return false;
    }
    // Opus accepts 2.5, 5, 10, 20, 40 and 60 ms. Anything else is rejected here
    // rather than at the first encode, so a bad configuration fails loudly at
    // construction instead of producing silence in a session.
    const std::size_t quarterMillisecond = static_cast<std::size_t>(sampleRate) / 400U;
    if (quarterMillisecond == 0U) {
        return false;
    }
    for (const std::size_t units : {1U, 2U, 4U, 8U, 16U, 24U}) {
        if (frameSamples == quarterMillisecond * units) {
            return true;
        }
    }
    return false;
}

} // namespace

OpusStreamEncoder::OpusStreamEncoder(
    std::uint32_t sampleRate,
    std::size_t frameSamples,
    std::uint32_t bitsPerSecond,
    Content content)
    : frameSamples_(frameSamples), bitsPerSecond_(bitsPerSecond) {
    if (!supportedFrame(sampleRate, frameSamples)) {
        return;
    }
    int error = OPUS_OK;
    // Restricted low delay is what makes this usable for playing together: it
    // runs CELT alone and drops the algorithmic delay to 2.5 ms.
    OpusEncoder* encoder = opus_encoder_create(
        static_cast<opus_int32>(sampleRate), 1, OPUS_APPLICATION_RESTRICTED_LOWDELAY, &error);
    if (error != OPUS_OK || encoder == nullptr) {
        if (encoder != nullptr) {
            opus_encoder_destroy(encoder);
        }
        return;
    }
    static_cast<void>(opus_encoder_ctl(
        encoder, OPUS_SET_BITRATE(static_cast<opus_int32>(bitsPerSecond))));
    // Constant bitrate, so the upstream figure quoted to a musician is one they
    // can check rather than an average they cannot.
    static_cast<void>(opus_encoder_ctl(encoder, OPUS_SET_VBR(0)));
    // One mono stream every 5 ms leaves ample headroom for the highest
    // analysis setting on any machine that can run an audio interface.
    static_cast<void>(opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(10)));
    static_cast<void>(opus_encoder_ctl(
        encoder,
        OPUS_SET_SIGNAL(content == Content::Voice ? OPUS_SIGNAL_VOICE : OPUS_SIGNAL_MUSIC)));
    // Full bandwidth. An instrument's air and a cymbal's top both live above
    // the band a speech-tuned encoder would keep.
    static_cast<void>(opus_encoder_ctl(encoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_FULLBAND)));
    // Forward error correction needs SILK and is unavailable in this mode. The
    // receiver's own concealment covers loss instead.
    static_cast<void>(opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(0)));
    // Discontinuous transmission would stop sending during quiet passages,
    // which the receive path would read as a stalled stream.
    static_cast<void>(opus_encoder_ctl(encoder, OPUS_SET_DTX(0)));
    encoder_ = encoder;
}

OpusStreamEncoder::~OpusStreamEncoder() { destroy(); }

OpusStreamEncoder::OpusStreamEncoder(OpusStreamEncoder&& other) noexcept
    : encoder_(std::exchange(other.encoder_, nullptr)),
      frameSamples_(other.frameSamples_),
      bitsPerSecond_(other.bitsPerSecond_) {}

OpusStreamEncoder& OpusStreamEncoder::operator=(OpusStreamEncoder&& other) noexcept {
    if (this != &other) {
        destroy();
        encoder_ = std::exchange(other.encoder_, nullptr);
        frameSamples_ = other.frameSamples_;
        bitsPerSecond_ = other.bitsPerSecond_;
    }
    return *this;
}

void OpusStreamEncoder::destroy() noexcept {
    if (encoder_ != nullptr) {
        opus_encoder_destroy(static_cast<OpusEncoder*>(encoder_));
        encoder_ = nullptr;
    }
}

std::size_t OpusStreamEncoder::encode(
    std::span<const float> frame,
    std::span<std::uint8_t> packet) noexcept {
    if (encoder_ == nullptr || frame.size() != frameSamples_ || packet.empty()) {
        return 0U;
    }
    const opus_int32 written = opus_encode_float(
        static_cast<OpusEncoder*>(encoder_),
        frame.data(),
        static_cast<int>(frameSamples_),
        packet.data(),
        static_cast<opus_int32>(std::min<std::size_t>(packet.size(), maximumPacketBytes())));
    return written > 0 ? static_cast<std::size_t>(written) : 0U;
}

bool OpusStreamEncoder::setBitsPerSecond(std::uint32_t bitsPerSecond) noexcept {
    if (encoder_ == nullptr || bitsPerSecond < 6'000U || bitsPerSecond > 510'000U) {
        return false;
    }
    if (opus_encoder_ctl(
            static_cast<OpusEncoder*>(encoder_),
            OPUS_SET_BITRATE(static_cast<opus_int32>(bitsPerSecond))) != OPUS_OK) {
        return false;
    }
    bitsPerSecond_ = bitsPerSecond;
    return true;
}

OpusStreamDecoder::OpusStreamDecoder(std::uint32_t sampleRate, std::size_t frameSamples)
    : frameSamples_(frameSamples) {
    if (!supportedFrame(sampleRate, frameSamples)) {
        return;
    }
    int error = OPUS_OK;
    OpusDecoder* decoder =
        opus_decoder_create(static_cast<opus_int32>(sampleRate), 1, &error);
    if (error != OPUS_OK || decoder == nullptr) {
        if (decoder != nullptr) {
            opus_decoder_destroy(decoder);
        }
        return;
    }
    decoder_ = decoder;
}

OpusStreamDecoder::~OpusStreamDecoder() { destroy(); }

OpusStreamDecoder::OpusStreamDecoder(OpusStreamDecoder&& other) noexcept
    : decoder_(std::exchange(other.decoder_, nullptr)), frameSamples_(other.frameSamples_) {}

OpusStreamDecoder& OpusStreamDecoder::operator=(OpusStreamDecoder&& other) noexcept {
    if (this != &other) {
        destroy();
        decoder_ = std::exchange(other.decoder_, nullptr);
        frameSamples_ = other.frameSamples_;
    }
    return *this;
}

void OpusStreamDecoder::destroy() noexcept {
    if (decoder_ != nullptr) {
        opus_decoder_destroy(static_cast<OpusDecoder*>(decoder_));
        decoder_ = nullptr;
    }
}

std::size_t OpusStreamDecoder::decode(
    std::span<const std::uint8_t> packet,
    std::span<float> frame) noexcept {
    if (decoder_ == nullptr || frame.size() != frameSamples_) {
        return 0U;
    }
    if (packet.empty()) {
        return conceal(frame);
    }
    const int produced = opus_decode_float(
        static_cast<OpusDecoder*>(decoder_),
        packet.data(),
        static_cast<opus_int32>(packet.size()),
        frame.data(),
        static_cast<int>(frameSamples_),
        0);
    if (produced <= 0) {
        std::fill(frame.begin(), frame.end(), 0.0F);
        return 0U;
    }
    return static_cast<std::size_t>(produced);
}

std::size_t OpusStreamDecoder::conceal(std::span<float> frame) noexcept {
    if (decoder_ == nullptr || frame.size() != frameSamples_) {
        return 0U;
    }
    // A null packet is how Opus is told a frame was lost. It advances the
    // decoder's own state and synthesises a replacement for the gap.
    const int produced = opus_decode_float(
        static_cast<OpusDecoder*>(decoder_),
        nullptr,
        0,
        frame.data(),
        static_cast<int>(frameSamples_),
        0);
    if (produced <= 0) {
        std::fill(frame.begin(), frame.end(), 0.0F);
        return 0U;
    }
    return static_cast<std::size_t>(produced);
}

void OpusStreamDecoder::reset() noexcept {
    if (decoder_ != nullptr) {
        static_cast<void>(
            opus_decoder_ctl(static_cast<OpusDecoder*>(decoder_), OPUS_RESET_STATE));
    }
}

} // namespace jamlink::audio
