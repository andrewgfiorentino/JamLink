// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/audio_block.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jamlink::audio {

enum class AudioBackendKind : std::uint8_t {
    Asio,
    WasapiExclusive,
    WasapiShared,
    Test
};

enum class AudioDeviceDirection : std::uint8_t {
    Input,
    Output,
    Duplex
};

struct AudioChannelInfo final {
    std::uint32_t index{0};
    std::string stableId;
    std::string displayName;
};

struct AudioBufferCapabilities final {
    std::uint32_t minimumFrames{0};
    std::uint32_t maximumFrames{0};
    std::uint32_t preferredFrames{0};
    std::uint32_t frameGranularity{0};
    std::vector<std::uint32_t> discreteFrameCounts;
};

struct AudioLatencyCapabilities final {
    std::optional<std::uint32_t> reportedInputFrames;
    std::optional<std::uint32_t> reportedOutputFrames;
};

struct AudioDeviceInfo final {
    std::string stableId;
    std::string displayName;
    AudioBackendKind backend{AudioBackendKind::Test};
    AudioDeviceDirection direction{AudioDeviceDirection::Duplex};
    std::vector<AudioChannelInfo> inputChannels;
    std::vector<AudioChannelInfo> outputChannels;
    std::vector<std::uint32_t> supportedSampleRates;
    AudioBufferCapabilities bufferCapabilities;
    AudioLatencyCapabilities latencyCapabilities;
    bool isDefault{false};
};

struct AudioStreamConfiguration final {
    std::string deviceId;
    std::uint32_t sampleRate{48'000};
    std::uint32_t bufferFrames{128};
    std::vector<std::uint32_t> inputChannelIndices;
    std::vector<std::uint32_t> outputChannelIndices;
    std::uint64_t clockDomainId{0};
};

struct AudioProcessBlock final {
    ConstInterleavedAudioBlock input;
    InterleavedAudioBlock output;
    std::uint64_t deviceFramePosition{0};
};

class IAudioProcessCallback {
public:
    virtual ~IAudioProcessCallback() = default;
    virtual void process(AudioProcessBlock block) noexcept = 0;
};

struct OpenedAudioStreamInfo final {
    std::uint32_t sampleRate{0};
    std::uint32_t bufferFrames{0};
    std::optional<std::uint32_t> inputLatencyFrames;
    std::optional<std::uint32_t> outputLatencyFrames;
};

struct AudioOpenResult final {
    bool succeeded{false};
    std::string errorMessage;
    std::optional<OpenedAudioStreamInfo> streamInfo;
};

// All methods except IAudioProcessCallback::process run on control threads.
// Implementations own platform APIs and must never expose them to the core.
class IAudioDeviceBackend {
public:
    virtual ~IAudioDeviceBackend() = default;

    [[nodiscard]] virtual AudioBackendKind kind() const noexcept = 0;
    [[nodiscard]] virtual std::vector<AudioDeviceInfo> enumerateDevices() = 0;
    [[nodiscard]] virtual AudioOpenResult open(
        const AudioStreamConfiguration& configuration,
        IAudioProcessCallback& callback) = 0;
    virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    virtual void close() noexcept = 0;
};

} // namespace jamlink::audio
