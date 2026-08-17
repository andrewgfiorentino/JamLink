// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/peer_audio_transport.hpp"

namespace jamlink::network {

std::unique_ptr<IPeerAudioTransport> createPlatformPeerAudioTransport() {
    return {};
}

} // namespace jamlink::network
