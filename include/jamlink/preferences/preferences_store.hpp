// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/preferences/user_preferences.hpp"

#include <filesystem>
#include <string>

namespace jamlink::preferences {

enum class PreferencesLoadState {
    Loaded,
    Missing,
    RecoveredDefaults
};

struct PreferencesLoadResult final {
    PreferencesLoadState state{PreferencesLoadState::Missing};
    UserPreferences preferences;
    std::string diagnostic;
};

struct PreferencesSaveResult final {
    bool succeeded{false};
    std::string diagnostic;
};

// Control-thread/file-worker only. Writes use a sibling temporary file and an
// atomic replace on Windows so an interrupted save cannot expose a partial file.
class PreferencesStore final {
public:
    explicit PreferencesStore(std::filesystem::path path);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;
    [[nodiscard]] PreferencesLoadResult load() const;
    [[nodiscard]] PreferencesSaveResult save(const UserPreferences& preferences) const;

private:
    std::filesystem::path path_;
};

} // namespace jamlink::preferences
