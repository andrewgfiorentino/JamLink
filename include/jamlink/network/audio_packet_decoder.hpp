// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace jamlink::network {

// Turns one received packet into playable audio.
//
// The receiver holds packets as they arrived and decodes at playout, in
// sequence order. That ordering is not an implementation detail. A predictive
// codec carries state from frame to frame, so decoding on arrival would corrupt
// that state on any reordering, and a gap the decoder is never told about makes
// the frames *after* it wrong as well as the frame that went missing.
//
// Implementations run on the audio callback. They must not allocate, lock, or
// touch a file or socket once constructed.
class IAudioPacketDecoder {
public:
    virtual ~IAudioPacketDecoder() = default;
    IAudioPacketDecoder(const IAudioPacketDecoder&) = delete;
    IAudioPacketDecoder& operator=(const IAudioPacketDecoder&) = delete;

    // Decodes into exactly frame.size() samples. Returns the samples produced;
    // zero means the packet was unusable, and the receiver conceals instead.
    [[nodiscard]] virtual std::size_t decode(
        std::span<const std::uint8_t> packet,
        std::span<float> frame) noexcept = 0;

    // Tells the decoder a packet was lost, so its state stays in step with the
    // encoder. Returns the samples written if this decoder synthesises its own
    // replacement, or zero to leave concealment to the receiver.
    //
    // Only called for audio that genuinely went missing. Stretching playout to
    // rebuild the buffer inserts a packet without losing one, and reporting
    // that as loss would advance the decoder past the encoder and corrupt
    // everything after it.
    [[nodiscard]] virtual std::size_t conceal(std::span<float> frame) noexcept = 0;

    virtual void reset() noexcept = 0;

    // Largest packet this decoder will be handed, which sizes the slot ring.
    [[nodiscard]] virtual std::size_t maximumPacketBytes() const noexcept = 0;

protected:
    IAudioPacketDecoder() = default;
    IAudioPacketDecoder(IAudioPacketDecoder&&) = default;
    IAudioPacketDecoder& operator=(IAudioPacketDecoder&&) = default;
};

// Uncompressed float samples, byte for byte.
//
// This is what the impairment tests run against, deliberately. Measuring the
// jitter buffer and its concealment against a codec's own concealment would
// measure the codec; with this decoder in place the buffer's behaviour is the
// only thing under test, and the results stay comparable to every measurement
// taken before a codec existed.
class PcmPassThroughDecoder final : public IAudioPacketDecoder {
public:
    explicit PcmPassThroughDecoder(std::size_t frameSamples) noexcept
        : frameSamples_(frameSamples) {}

    [[nodiscard]] std::size_t decode(
        std::span<const std::uint8_t> packet,
        std::span<float> frame) noexcept override {
        if (frame.size() != frameSamples_
            || packet.size() != frameSamples_ * sizeof(float)) {
            return 0U;
        }
        std::memcpy(frame.data(), packet.data(), packet.size());
        return frameSamples_;
    }

    // No codec state to keep in step, and no concealment of its own: the
    // receiver's pitch-synchronous extrapolation covers the gap.
    [[nodiscard]] std::size_t conceal(std::span<float>) noexcept override { return 0U; }

    void reset() noexcept override {}

    [[nodiscard]] std::size_t maximumPacketBytes() const noexcept override {
        return frameSamples_ * sizeof(float);
    }

private:
    std::size_t frameSamples_;
};

} // namespace jamlink::network
