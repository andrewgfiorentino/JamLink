// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/soundcheck_audio_service.hpp"

namespace jamlink::audio {

std::unique_ptr<ISoundcheckAudioService> createPlatformSoundcheckAudioService() {
    return {};
}

} // namespace jamlink::audio
