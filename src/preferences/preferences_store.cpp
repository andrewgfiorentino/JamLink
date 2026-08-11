// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/preferences/preferences_store.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#endif

namespace jamlink::preferences {
namespace {

constexpr std::string_view fileHeader = "JAMLINK_PREFERENCES 1";

bool readQuotedLine(
    std::istream& input,
    std::string_view expectedKey,
    std::string& value) {
    std::string line;
    if (!std::getline(input, line)) {
        return false;
    }
    std::istringstream parser(line);
    std::string key;
    if (!(parser >> key >> std::quoted(value))) {
        return false;
    }
    parser >> std::ws;
    return key == expectedKey && parser.peek() == std::char_traits<char>::eof();
}

// Reads a quoted value from an already-split optional line.
bool readQuotedValue(std::istream& line, std::string& value) {
    if (!(line >> std::quoted(value))) {
        return false;
    }
    line >> std::ws;
    return line.peek() == std::char_traits<char>::eof();
}

template <typename Value>
bool readScalarLine(
    std::istream& input,
    std::string_view expectedKey,
    Value& value) {
    std::string line;
    if (!std::getline(input, line)) {
        return false;
    }
    std::istringstream parser(line);
    std::string key;
    if (!(parser >> key >> value)) {
        return false;
    }
    parser >> std::ws;
    return key == expectedKey && parser.peek() == std::char_traits<char>::eof();
}

bool valid(const UserPreferences& preferences) noexcept {
    const bool sampleRateValid = preferences.sampleRate >= 8'000U
        && preferences.sampleRate <= 384'000U;
    const bool bufferValid = preferences.bufferFrames >= 1U
        && preferences.bufferFrames <= 8'192U;
    const bool gainsValid = std::isfinite(preferences.instrumentMonitorGain)
        && std::isfinite(preferences.voiceMonitorGain)
        && preferences.instrumentMonitorGain >= 0.0F
        && preferences.instrumentMonitorGain <= 2.0F
        && preferences.voiceMonitorGain >= 0.0F
        && preferences.voiceMonitorGain <= 2.0F
        && preferences.remoteInstrumentGain >= 0.0F
        && preferences.remoteInstrumentGain <= 2.0F
        && preferences.remoteVoiceGain >= 0.0F
        && preferences.remoteVoiceGain <= 2.0F;
    const bool networkValid = preferences.preferredUdpPort <= 65'535U
        && preferences.latencyMode <= 2U;
    const bool windowValid = preferences.window.width >= 532U
        && preferences.window.width <= 16'384U
        && preferences.window.height >= 480U
        && preferences.window.height <= 16'384U;
    return preferences.schemaVersion == currentPreferencesSchemaVersion
        && sampleRateValid && bufferValid && gainsValid && windowValid && networkValid;
}

bool parse(std::istream& input, UserPreferences& preferences) {
    std::string header;
    if (!std::getline(input, header) || header != fileHeader) {
        return false;
    }

    int instrumentEnabled = 0;
    int voiceEnabled = 0;
    int hasPosition = 0;
    if (!readQuotedLine(input, "instrument.device", preferences.instrument.deviceId)
        || !readQuotedLine(
            input, "instrument.channel.primary", preferences.instrument.primaryChannelId)
        || !readQuotedLine(
            input, "instrument.channel.secondary", preferences.instrument.secondaryChannelId)
        || !readQuotedLine(input, "voice.device", preferences.voice.deviceId)
        || !readQuotedLine(input, "voice.channel.primary", preferences.voice.primaryChannelId)
        || !readQuotedLine(input, "voice.channel.secondary", preferences.voice.secondaryChannelId)
        || !readQuotedLine(input, "output.device", preferences.output.deviceId)
        || !readQuotedLine(input, "output.channel.primary", preferences.output.primaryChannelId)
        || !readQuotedLine(input, "output.channel.secondary", preferences.output.secondaryChannelId)
        || !readScalarLine(input, "audio.sample_rate", preferences.sampleRate)
        || !readScalarLine(input, "audio.buffer_frames", preferences.bufferFrames)
        || !readScalarLine(
            input, "monitor.instrument.gain", preferences.instrumentMonitorGain)
        || !readScalarLine(input, "monitor.voice.gain", preferences.voiceMonitorGain)
        || !readScalarLine(input, "monitor.instrument.enabled", instrumentEnabled)
        || !readScalarLine(input, "monitor.voice.enabled", voiceEnabled)
        || !readScalarLine(input, "window.x", preferences.window.x)
        || !readScalarLine(input, "window.y", preferences.window.y)
        || !readScalarLine(input, "window.width", preferences.window.width)
        || !readScalarLine(input, "window.height", preferences.window.height)
        || !readScalarLine(input, "window.has_position", hasPosition)) {
        return false;
    }

    if ((instrumentEnabled != 0 && instrumentEnabled != 1)
        || (voiceEnabled != 0 && voiceEnabled != 1)
        || (hasPosition != 0 && hasPosition != 1)) {
        return false;
    }
    preferences.instrumentMonitorEnabled = instrumentEnabled == 1;
    preferences.voiceMonitorEnabled = voiceEnabled == 1;
    preferences.window.hasPosition = hasPosition == 1;

    // Optional trailing keys. Absent in files written by earlier builds, which
    // must keep restoring rather than being discarded as corrupt. Unknown keys
    // are still rejected.
    int tunerMutes = preferences.tunerMutesInstrument ? 1 : 0;
    int automaticMapping = preferences.automaticPortMapping ? 1 : 0;
    std::string trailing;
    while (std::getline(input, trailing)) {
        if (trailing.empty()) {
            continue;
        }
        std::istringstream line(trailing);
        std::string key;
        line >> key;
        bool read = false;
        if (key == "room.remote.instrument.gain") {
            read = static_cast<bool>(line >> preferences.remoteInstrumentGain);
        } else if (key == "room.remote.voice.gain") {
            read = static_cast<bool>(line >> preferences.remoteVoiceGain);
        } else if (key == "tuner.mutes_instrument") {
            read = static_cast<bool>(line >> tunerMutes);
        } else if (key == "record.directory") {
            read = readQuotedValue(line, preferences.recordingDirectory);
        } else if (key == "network.port") {
            read = static_cast<bool>(line >> preferences.preferredUdpPort);
        } else if (key == "network.automatic_mapping") {
            read = static_cast<bool>(line >> automaticMapping);
        } else if (key == "network.latency_mode") {
            read = static_cast<bool>(line >> preferences.latencyMode);
        }
        if (!read) {
            return false;
        }
    }
    if ((tunerMutes != 0 && tunerMutes != 1)
        || (automaticMapping != 0 && automaticMapping != 1)) {
        return false;
    }
    preferences.tunerMutesInstrument = tunerMutes == 1;
    preferences.automaticPortMapping = automaticMapping == 1;
    return valid(preferences);
}

void write(std::ostream& output, const UserPreferences& preferences) {
    output << fileHeader << '\n'
           << "instrument.device " << std::quoted(preferences.instrument.deviceId) << '\n'
           << "instrument.channel.primary "
           << std::quoted(preferences.instrument.primaryChannelId) << '\n'
           << "instrument.channel.secondary "
           << std::quoted(preferences.instrument.secondaryChannelId) << '\n'
           << "voice.device " << std::quoted(preferences.voice.deviceId) << '\n'
           << "voice.channel.primary " << std::quoted(preferences.voice.primaryChannelId) << '\n'
           << "voice.channel.secondary " << std::quoted(preferences.voice.secondaryChannelId) << '\n'
           << "output.device " << std::quoted(preferences.output.deviceId) << '\n'
           << "output.channel.primary " << std::quoted(preferences.output.primaryChannelId) << '\n'
           << "output.channel.secondary " << std::quoted(preferences.output.secondaryChannelId) << '\n'
           << "audio.sample_rate " << preferences.sampleRate << '\n'
           << "audio.buffer_frames " << preferences.bufferFrames << '\n'
           << "monitor.instrument.gain "
           << std::setprecision(std::numeric_limits<float>::max_digits10)
           << preferences.instrumentMonitorGain << '\n'
           << "monitor.voice.gain " << preferences.voiceMonitorGain << '\n'
           << "monitor.instrument.enabled "
           << static_cast<int>(preferences.instrumentMonitorEnabled) << '\n'
           << "monitor.voice.enabled "
           << static_cast<int>(preferences.voiceMonitorEnabled) << '\n'
           << "window.x " << preferences.window.x << '\n'
           << "window.y " << preferences.window.y << '\n'
           << "window.width " << preferences.window.width << '\n'
           << "window.height " << preferences.window.height << '\n'
           << "window.has_position " << static_cast<int>(preferences.window.hasPosition) << '\n'
           << "room.remote.instrument.gain " << preferences.remoteInstrumentGain << '\n'
           << "room.remote.voice.gain " << preferences.remoteVoiceGain << '\n'
           << "tuner.mutes_instrument "
           << static_cast<int>(preferences.tunerMutesInstrument) << '\n'
           << "record.directory " << std::quoted(preferences.recordingDirectory) << '\n'
           << "network.port " << preferences.preferredUdpPort << '\n'
           << "network.automatic_mapping "
           << static_cast<int>(preferences.automaticPortMapping) << '\n'
           << "network.latency_mode " << preferences.latencyMode << '\n';
}

PreferencesSaveResult replaceFile(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination) {
#if defined(_WIN32)
    if (MoveFileExW(
            temporary.c_str(),
            destination.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
        return {false, "Atomic preference replace failed with Windows error "
                + std::to_string(GetLastError())};
    }
    return {true, {}};
#else
    std::error_code error;
    std::filesystem::rename(temporary, destination, error);
    if (error) {
        return {false, "Atomic preference replace failed: " + error.message()};
    }
    return {true, {}};
#endif
}

} // namespace

PreferencesStore::PreferencesStore(std::filesystem::path path)
    : path_(std::move(path)) {}

const std::filesystem::path& PreferencesStore::path() const noexcept {
    return path_;
}

PreferencesLoadResult PreferencesStore::load() const {
    std::error_code existenceError;
    const bool exists = std::filesystem::exists(path_, existenceError);
    if (!exists && !existenceError) {
        return {PreferencesLoadState::Missing, {}, {}};
    }
    if (existenceError) {
        return {PreferencesLoadState::RecoveredDefaults, {},
                "Could not inspect preference file: " + existenceError.message()};
    }

    std::ifstream input(path_, std::ios::binary);
    if (!input) {
        return {PreferencesLoadState::RecoveredDefaults, {},
                "Could not open preference file"};
    }

    UserPreferences preferences;
    if (!parse(input, preferences)) {
        return {PreferencesLoadState::RecoveredDefaults, {},
                "Preference file is invalid or uses an unsupported schema"};
    }
    return {PreferencesLoadState::Loaded, std::move(preferences), {}};
}

PreferencesSaveResult PreferencesStore::save(const UserPreferences& preferences) const {
    if (!valid(preferences)) {
        return {false, "Preferences failed validation"};
    }

    std::error_code directoryError;
    const auto parent = path_.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, directoryError);
    }
    if (directoryError) {
        return {false, "Could not create preference directory: " + directoryError.message()};
    }

    auto temporary = path_;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return {false, "Could not open temporary preference file"};
        }
        write(output, preferences);
        output.flush();
        if (!output) {
            return {false, "Could not write temporary preference file"};
        }
    }

    auto result = replaceFile(temporary, path_);
    if (!result.succeeded) {
        std::error_code cleanupError;
        std::filesystem::remove(temporary, cleanupError);
    }
    return result;
}

} // namespace jamlink::preferences
