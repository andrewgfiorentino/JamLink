// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>

namespace jamlink::audio {

// Which Windows audio path an endpoint is reached through. Split into its own
// header because both the audio service and the topology rules need it, and
// having either include the other would be a cycle.
enum class SoundcheckBackend : std::uint8_t {
    WasapiShared,
    Asio
};

} // namespace jamlink::audio
