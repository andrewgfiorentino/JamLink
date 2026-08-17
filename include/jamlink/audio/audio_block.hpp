// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <span>

namespace jamlink::audio {

struct ConstInterleavedAudioBlock final {
    std::span<const float> samples;
    std::size_t channelCount{0};

    [[nodiscard]] std::size_t frameCount() const noexcept {
        return channelCount == 0 ? 0 : samples.size() / channelCount;
    }

    [[nodiscard]] bool valid() const noexcept {
        return channelCount != 0 && samples.size() % channelCount == 0;
    }
};

struct InterleavedAudioBlock final {
    std::span<float> samples;
    std::size_t channelCount{0};

    [[nodiscard]] std::size_t frameCount() const noexcept {
        return channelCount == 0 ? 0 : samples.size() / channelCount;
    }

    [[nodiscard]] bool valid() const noexcept {
        return channelCount != 0 && samples.size() % channelCount == 0;
    }
};

} // namespace jamlink::audio
