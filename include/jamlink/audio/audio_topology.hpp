// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/soundcheck_backend.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace jamlink::audio {

// Whether three chosen devices can actually run together, and if not, the one
// thing to change.
//
// This exists because of a real two-home field test. A musician selected
// "Focusrite USB ASIO - Input 1" for guitar, a Yeti for voice, and
// "Speakers (Focusrite USB Audio)" for output. Every one of those is a real,
// working endpoint, and two of them are the same physical box. The graph is
// still impossible: an ASIO driver owns the whole interface, so its input
// cannot be driven from an ASIO callback while its output is opened through
// Windows shared audio.
//
// JamLink rejected it inside audio startup and reported "unsupported Windows
// format", which was actively misleading -- no format was unsupported. The
// session log showed sample rate 0 and buffer 0 because the engine never
// started, and there was nothing to say why.
//
// The rules are therefore stated once, here, where they can be tested and
// where the interface, the audio service, and the support bundle all read the
// same answer instead of each inventing one.
enum class AudioTopologyReason : std::uint8_t {
    ValidWasapiGraph,
    ValidAsioGraph,
    // The configuration this whole file exists to make work: an ASIO interface
    // for guitar and headphones, with an independently clocked USB microphone
    // bridged into the ASIO clock domain.
    ValidAsioWithSecondaryWasapiVoice,
    // An ASIO driver owns the interface. Its input cannot be used while its
    // output is opened through Windows shared audio.
    AsioInstrumentRequiresAsioOutput,
    AsioOutputRequiresAsioInstrument,
    // Two different ASIO drivers cannot both be the master clock.
    AsioDriverMismatch,
    AsioVoiceDriverMismatch,
    MissingEndpoint,
};

[[nodiscard]] std::string_view topologyReasonName(AudioTopologyReason reason) noexcept;

struct AudioTopologyEndpoint final {
    SoundcheckBackend backend{SoundcheckBackend::WasapiShared};
    // For ASIO this is "asio:" plus the driver name, so two endpoints belong to
    // the same driver exactly when their identifiers match.
    std::string endpointId;
    std::string displayName;

    [[nodiscard]] bool present() const noexcept { return !endpointId.empty(); }
    [[nodiscard]] bool isAsio() const noexcept {
        return backend == SoundcheckBackend::Asio;
    }
};

struct AudioTopology final {
    AudioTopologyEndpoint instrument;
    AudioTopologyEndpoint voice;
    AudioTopologyEndpoint output;
};

struct AudioTopologyResult final {
    bool supported{false};
    // Which backend owns the realtime clock everything else is bridged into.
    SoundcheckBackend masterBackend{SoundcheckBackend::WasapiShared};
    // The voice arrives on its own clock and is bridged into the master's.
    bool hybridVoice{false};
    AudioTopologyReason reason{AudioTopologyReason::MissingEndpoint};

    // The one thing to change, so the interface can offer a single action
    // rather than a description of the problem.
    bool changeOutput{false};
    bool changeInstrument{false};
    bool changeVoice{false};
    // The driver the offending endpoint has to come from, when there is one.
    std::string requiredEndpointId;

    // Musician language. No mention of backends, drivers or clock domains.
    [[nodiscard]] std::string_view advice() const noexcept;
};

[[nodiscard]] AudioTopologyResult evaluateAudioTopology(const AudioTopology& topology);

} // namespace jamlink::audio
