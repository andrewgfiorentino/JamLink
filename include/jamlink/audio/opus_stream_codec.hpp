// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace jamlink::audio {

// One mono stream, encoded for the wire.
//
// JamLink sends guitar and voice as two independent streams of 5 ms packets.
// Uncompressed that is roughly 1.6 Mbit/s upstream, which is more than many
// domestic uplinks will carry alongside anything else, and more than twice what
// comparable applications use.
//
// Opus is configured here for playing together rather than for listening:
//
//   Restricted low delay. This disables the SILK layer entirely and runs CELT
//   alone, which drops the codec's algorithmic delay to 2.5 ms. The default
//   mode would add 6.5 ms in each direction -- more than the entire monitoring
//   path on an ASIO interface, and enough to be felt while playing.
//
//   Five millisecond frames, matching the 240-sample packet the transport
//   already sends. No repacketisation, and no change to the send schedule.
//
//   Constant bitrate. Variable bitrate would sound slightly better for the same
//   average, but the average is not what matters here: a stated upstream figure
//   should be one a musician can check against their connection, and a burst
//   that overruns a thin uplink is heard as loss.
//
// Both classes allocate when constructed and never afterwards, so they are safe
// on the network worker and, if it comes to it, on the audio callback.
class OpusStreamEncoder final {
public:
    // Guitar and voice want different tuning. Music preserves harmonic detail;
    // voice spends its bits on intelligibility.
    enum class Content : std::uint8_t { Music, Voice };

    OpusStreamEncoder(
        std::uint32_t sampleRate,
        std::size_t frameSamples,
        std::uint32_t bitsPerSecond,
        Content content);
    ~OpusStreamEncoder();

    OpusStreamEncoder(const OpusStreamEncoder&) = delete;
    OpusStreamEncoder& operator=(const OpusStreamEncoder&) = delete;
    OpusStreamEncoder(OpusStreamEncoder&&) noexcept;
    OpusStreamEncoder& operator=(OpusStreamEncoder&&) noexcept;

    // Encodes exactly frameSamples into `packet`. Returns the bytes written, or
    // zero if the frame was the wrong size or the encoder refused it.
    [[nodiscard]] std::size_t encode(
        std::span<const float> frame,
        std::span<std::uint8_t> packet) noexcept;

    [[nodiscard]] bool setBitsPerSecond(std::uint32_t bitsPerSecond) noexcept;
    [[nodiscard]] std::uint32_t bitsPerSecond() const noexcept { return bitsPerSecond_; }
    [[nodiscard]] bool valid() const noexcept { return encoder_ != nullptr; }

    // Largest packet this configuration can produce, for sizing buffers.
    [[nodiscard]] static std::size_t maximumPacketBytes() noexcept { return 1'275U; }

private:
    void destroy() noexcept;

    void* encoder_{nullptr};
    std::size_t frameSamples_{0U};
    std::uint32_t bitsPerSecond_{0U};
};

class OpusStreamDecoder final {
public:
    OpusStreamDecoder(std::uint32_t sampleRate, std::size_t frameSamples);
    ~OpusStreamDecoder();

    OpusStreamDecoder(const OpusStreamDecoder&) = delete;
    OpusStreamDecoder& operator=(const OpusStreamDecoder&) = delete;
    OpusStreamDecoder(OpusStreamDecoder&&) noexcept;
    OpusStreamDecoder& operator=(OpusStreamDecoder&&) noexcept;

    // Decodes one packet into exactly frameSamples. Returns samples produced,
    // or zero on failure, in which case `frame` is left silent.
    [[nodiscard]] std::size_t decode(
        std::span<const std::uint8_t> packet,
        std::span<float> frame) noexcept;

    // For a packet that never arrived.
    //
    // This is not the same as writing silence, and it is not optional. Opus
    // carries prediction across frames, so a decoder that is never told about a
    // gap keeps predicting from state the encoder has already moved past, and
    // the packets after the gap decode wrong as well. Calling this keeps the
    // two ends in step and produces the codec's own concealment for the missing
    // frame at the same time.
    [[nodiscard]] std::size_t conceal(std::span<float> frame) noexcept;

    void reset() noexcept;
    [[nodiscard]] bool valid() const noexcept { return decoder_ != nullptr; }

private:
    void destroy() noexcept;

    void* decoder_{nullptr};
    std::size_t frameSamples_{0U};
};

} // namespace jamlink::audio
