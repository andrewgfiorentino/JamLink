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
    // Sized to the single-column layout. The old 1400x900 default stretched a
    // narrow design across a wide window on first launch.
    std::uint32_t width{560U};
    std::uint32_t height{780U};
    bool hasPosition{false};

    bool operator==(const WindowPlacement&) const = default;
};

struct MusicianProfile final {
    // Stable generated identity. Handles and display names may change without
    // changing the identity exchanged in authenticated rooms.
    std::string profileId;
    std::string handle;
    std::string displayName{"Musician"};
    std::string avatarId{"avatar:guitar-electric"};
    std::string customAvatarPath;
    std::string primaryInstrument{"Guitar"};
    std::string genres;
    std::string bio;
    std::string region;
    bool shareInstrument{true};
    bool shareGenres{true};
    bool shareBio{false};
    bool shareRegion{false};

    bool operator==(const MusicianProfile&) const = default;
};

// Readiness is deliberately excluded. A restored device selection must be
// revalidated before it can be treated as safe for room entry.
struct UserPreferences final {
    std::uint32_t schemaVersion{currentPreferencesSchemaVersion};
    AudioSelection instrument;
    AudioSelection voice;
    AudioSelection output;
    std::uint32_t sampleRate{48'000U};
    // Zero selects the automatic buffer size. A musician who does not know
    // what a buffer is should not have to guess, and guessing high is what
    // makes playing together feel late.
    std::uint32_t bufferFrames{0U};
    float instrumentMonitorGain{0.72F};
    float voiceMonitorGain{0.55F};
    bool instrumentMonitorEnabled{false};
    bool voiceMonitorEnabled{false};
    // How loudly this user hears each of a friend's streams, and whether the
    // tuner silences the instrument to the room. Appended after the fields
    // above and treated as optional on read, so a preferences file written by
    // an earlier build still restores everything it did before.
    float remoteInstrumentGain{1.0F};
    float remoteVoiceGain{1.0F};
    bool tunerMutesInstrument{true};
    // Where takes are written. Empty means the platform music folder.
    std::string recordingDirectory;
    // Zero asks the operating system for any free UDP port.
    std::uint32_t preferredUdpPort{0U};
    bool automaticPortMapping{true};
    // How the receive buffer trades latency against tolerance of a rough
    // network: 0 lowest latency, 1 balanced, 2 most stable.
    std::uint32_t latencyMode{1U};
    MusicianProfile profile;
    WindowPlacement window;

    bool operator==(const UserPreferences&) const = default;
};

} // namespace jamlink::preferences
