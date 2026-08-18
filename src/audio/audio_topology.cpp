// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/audio_topology.hpp"

namespace jamlink::audio {

std::string_view topologyReasonName(AudioTopologyReason reason) noexcept {
    switch (reason) {
    case AudioTopologyReason::ValidWasapiGraph: return "ValidWasapiGraph";
    case AudioTopologyReason::ValidAsioGraph: return "ValidAsioGraph";
    case AudioTopologyReason::ValidAsioWithSecondaryWasapiVoice:
        return "ValidAsioWithSecondaryWasapiVoice";
    case AudioTopologyReason::AsioInstrumentRequiresAsioOutput:
        return "AsioInstrumentRequiresAsioOutput";
    case AudioTopologyReason::AsioOutputRequiresAsioInstrument:
        return "AsioOutputRequiresAsioInstrument";
    case AudioTopologyReason::AsioDriverMismatch: return "AsioDriverMismatch";
    case AudioTopologyReason::AsioVoiceDriverMismatch: return "AsioVoiceDriverMismatch";
    case AudioTopologyReason::MissingEndpoint: return "MissingEndpoint";
    }
    return "Unknown";
}

std::string_view AudioTopologyResult::advice() const noexcept {
    switch (reason) {
    case AudioTopologyReason::ValidWasapiGraph:
    case AudioTopologyReason::ValidAsioGraph:
        return "";
    case AudioTopologyReason::ValidAsioWithSecondaryWasapiVoice:
        return "";
    case AudioTopologyReason::AsioInstrumentRequiresAsioOutput:
        return "Your guitar is using your interface's own low-latency driver, but "
               "your headphones are going through Windows. Switch the output to the "
               "same interface and your microphone can stay exactly as it is.";
    case AudioTopologyReason::AsioOutputRequiresAsioInstrument:
        return "Your headphones are using your interface's own low-latency driver, "
               "but your guitar is going through Windows. Switch the guitar input to "
               "the same interface.";
    case AudioTopologyReason::AsioDriverMismatch:
        return "Your guitar and your headphones are on two different audio "
               "interfaces, and only one of them can run at a time. Choose the same "
               "interface for both.";
    case AudioTopologyReason::AsioVoiceDriverMismatch:
        return "Your microphone is on a different audio interface from your guitar. "
               "Use the same interface, or choose the microphone's plain Windows "
               "entry instead.";
    case AudioTopologyReason::MissingEndpoint:
        return "Choose an input for your guitar, a microphone, and an output.";
    }
    return "";
}

AudioTopologyResult evaluateAudioTopology(const AudioTopology& topology) {
    AudioTopologyResult result;

    if (!topology.instrument.present() || !topology.voice.present()
        || !topology.output.present()) {
        result.reason = AudioTopologyReason::MissingEndpoint;
        return result;
    }

    const auto& instrument = topology.instrument;
    const auto& voice = topology.voice;
    const auto& output = topology.output;

    // Everything through Windows shared audio. Always workable, never the
    // lowest delay, and the configuration a machine without an interface has.
    if (!instrument.isAsio() && !voice.isAsio() && !output.isAsio()) {
        result.supported = true;
        result.masterBackend = SoundcheckBackend::WasapiShared;
        result.reason = AudioTopologyReason::ValidWasapiGraph;
        return result;
    }

    // An ASIO driver takes exclusive ownership of its interface, so the guitar
    // and the headphones have to come from the same one. Whichever of the two
    // is already on ASIO decides which driver that is.
    if (instrument.isAsio() && !output.isAsio()) {
        result.reason = AudioTopologyReason::AsioInstrumentRequiresAsioOutput;
        result.changeOutput = true;
        result.requiredEndpointId = instrument.endpointId;
        return result;
    }
    if (output.isAsio() && !instrument.isAsio()) {
        result.reason = AudioTopologyReason::AsioOutputRequiresAsioInstrument;
        result.changeInstrument = true;
        result.requiredEndpointId = output.endpointId;
        return result;
    }
    if (instrument.endpointId != output.endpointId) {
        // Both ASIO, two different drivers. Only one can hold the clock.
        result.reason = AudioTopologyReason::AsioDriverMismatch;
        result.changeOutput = true;
        result.requiredEndpointId = instrument.endpointId;
        return result;
    }

    result.masterBackend = SoundcheckBackend::Asio;

    if (!voice.isAsio()) {
        // The configuration this file exists for. The microphone runs on its own
        // hardware clock and is bridged into the interface's, which is why the
        // interface stays the master and the microphone does not need to be an
        // ASIO device at all.
        result.supported = true;
        result.hybridVoice = true;
        result.reason = AudioTopologyReason::ValidAsioWithSecondaryWasapiVoice;
        return result;
    }
    if (voice.endpointId != instrument.endpointId) {
        // An ASIO microphone on a second interface would need a second master.
        // Its plain Windows entry works instead, through the bridge above.
        result.reason = AudioTopologyReason::AsioVoiceDriverMismatch;
        result.changeVoice = true;
        result.requiredEndpointId = instrument.endpointId;
        return result;
    }

    result.supported = true;
    result.reason = AudioTopologyReason::ValidAsioGraph;
    return result;
}

} // namespace jamlink::audio
