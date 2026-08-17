// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace jamlink::audio {

using AudioBusId = std::uint16_t;

enum class AudioGraphPurpose : std::uint8_t {
    PrivateSoundcheck,
    Session
};

enum class AudioBusRole : std::uint8_t {
    HardwareInput,
    LocalMusic,
    LocalVoice,
    RemoteMusic,
    RemoteVoice,
    Monitor,
    Cue,
    NetworkSend,
    NetworkReceive,
    Record,
    DawBridge,
    Talkback,
    Click,
    HardwareOutput
};

struct AudioBusDefinition final {
    AudioBusId id{0};
    AudioBusRole role{AudioBusRole::HardwareInput};
    std::size_t channelCount{0};
};

struct AudioRouteDefinition final {
    AudioBusId sourceBus{0};
    std::size_t sourceChannel{0};
    AudioBusId destinationBus{0};
    std::size_t destinationChannel{0};
    float linearGain{1.0F};
};

// A control thread compiles bus/route definitions and allocates all storage in
// the constructor. beginBlock(), bus access, and processRoutes() are bounded and
// allocation-free for use by one real-time graph owner.
class AudioRouteGraph final {
public:
    AudioRouteGraph(
        AudioGraphPurpose purpose,
        std::size_t maximumFrames,
        std::span<const AudioBusDefinition> buses,
        std::span<const AudioRouteDefinition> routes);

    AudioRouteGraph(const AudioRouteGraph&) = delete;
    AudioRouteGraph& operator=(const AudioRouteGraph&) = delete;
    AudioRouteGraph(AudioRouteGraph&&) noexcept = default;
    AudioRouteGraph& operator=(AudioRouteGraph&&) noexcept = default;

    [[nodiscard]] bool beginBlock(std::size_t frameCount) noexcept;
    [[nodiscard]] std::span<float> mutableBus(AudioBusId id) noexcept;
    [[nodiscard]] std::span<const float> bus(AudioBusId id) const noexcept;
    void processRoutes() noexcept;

    [[nodiscard]] std::size_t maximumFrames() const noexcept { return maximumFrames_; }
    [[nodiscard]] std::size_t currentFrames() const noexcept { return currentFrames_; }
    [[nodiscard]] AudioGraphPurpose purpose() const noexcept { return purpose_; }

private:
    struct BusStorage final {
        AudioBusDefinition definition;
        std::vector<float> samples;
    };

    struct CompiledRoute final {
        std::size_t sourceBusIndex{0};
        std::size_t sourceChannel{0};
        std::size_t destinationBusIndex{0};
        std::size_t destinationChannel{0};
        float linearGain{1.0F};
    };

    [[nodiscard]] std::size_t findBusIndex(AudioBusId id) const noexcept;

    AudioGraphPurpose purpose_{AudioGraphPurpose::PrivateSoundcheck};
    std::size_t maximumFrames_{0};
    std::size_t currentFrames_{0};
    std::vector<BusStorage> buses_;
    std::vector<CompiledRoute> routes_;
};

} // namespace jamlink::audio
