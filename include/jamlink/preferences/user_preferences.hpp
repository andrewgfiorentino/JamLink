// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace jamlink::preferences {

inline constexpr std::uint32_t currentPreferencesSchemaVersion = 1U;

struct AudioSelection final {
    std::string deviceId;
    std::string primaryChannelId;
    std::string secondaryChannelId;

    bool operator==(const AudioSelection&) const = default;
};

struct WindowPlacement final {
    std::int32_t x{0};
    std::int32_t y{0};
    std::uint32_t width{1'400U};
    std::uint32_t height{900U};
    bool hasPosition{false};

    bool operator==(const WindowPlacement&) const = default;
};

// Readiness is deliberately excluded. A restored device selection must be
// revalidated before it can be treated as safe for room entry.
struct UserPreferences final {
    std::uint32_t schemaVersion{currentPreferencesSchemaVersion};
    AudioSelection instrument;
    AudioSelection voice;
    AudioSelection output;
    std::uint32_t sampleRate{48'000U};
    std::uint32_t bufferFrames{128U};
    float instrumentMonitorGain{0.72F};
    float voiceMonitorGain{0.55F};
    bool instrumentMonitorEnabled{true};
    bool voiceMonitorEnabled{true};
    WindowPlacement window;

    bool operator==(const UserPreferences&) const = default;
};

} // namespace jamlink::preferences
