// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/private_soundcheck_processor.hpp"

#include <algorithm>
#include <cmath>

namespace jamlink::audio {

PrivateSoundcheckProcessor::PrivateSoundcheckProcessor() noexcept = default;

void PrivateSoundcheckProcessor::setMonitorEnabled(
    SoundcheckSource source,
    bool enabled) noexcept {
    auto& state = source == SoundcheckSource::Instrument ? instrument_ : voice_;
    state.enabled.store(enabled ? 1U : 0U, std::memory_order_release);
}

void PrivateSoundcheckProcessor::setMonitorGain(
    SoundcheckSource source,
    float linearGain) noexcept {
    auto& state = source == SoundcheckSource::Instrument ? instrument_ : voice_;
    state.targetGain.store(sanitizeGain(linearGain));
}

void PrivateSoundcheckProcessor::process(
    const PrivateSoundcheckInputs& inputs,
    std::span<float> stereoMonitorOutput) noexcept {
    std::fill(stereoMonitorOutput.begin(), stereoMonitorOutput.end(), 0.0F);
    if (stereoMonitorOutput.size() % 2U != 0U) {
        return;
    }

    const std::size_t frames = stereoMonitorOutput.size() / 2U;
    if (frames == 0U) {
        return;
    }

    const float instrumentTarget = nextGain(instrument_);
    const float voiceTarget = nextGain(voice_);
    const float instrumentStep = (instrumentTarget - instrument_.currentGain)
        / static_cast<float>(frames);
    const float voiceStep = (voiceTarget - voice_.currentGain)
        / static_cast<float>(frames);

    float instrumentGain = instrument_.currentGain;
    float voiceGain = voice_.currentGain;
    for (std::size_t frame = 0; frame < frames; ++frame) {
        instrumentGain += instrumentStep;
        voiceGain += voiceStep;

        const float rawInstrumentSample = frame < inputs.instrumentMono.size()
            ? inputs.instrumentMono[frame]
            : 0.0F;
        const float rawVoiceSample = frame < inputs.voiceMono.size()
            ? inputs.voiceMono[frame]
            : 0.0F;
        const float instrumentSample = std::isfinite(rawInstrumentSample)
            ? rawInstrumentSample
            : 0.0F;
        const float voiceSample = std::isfinite(rawVoiceSample) ? rawVoiceSample : 0.0F;
        const float mixed = instrumentSample * instrumentGain + voiceSample * voiceGain;
        const float monitorSample = std::isfinite(mixed) ? mixed : 0.0F;

        stereoMonitorOutput[frame * 2U] = monitorSample;
        stereoMonitorOutput[frame * 2U + 1U] = monitorSample;
    }

    instrument_.currentGain = instrumentTarget;
    voice_.currentGain = voiceTarget;
}

void PrivateSoundcheckProcessor::reset() noexcept {
    instrument_.currentGain = 0.0F;
    voice_.currentGain = 0.0F;
}

float PrivateSoundcheckProcessor::sanitizeGain(float gain) noexcept {
    if (!std::isfinite(gain)) {
        return 0.0F;
    }
    return std::clamp(gain, 0.0F, 16.0F);
}

float PrivateSoundcheckProcessor::nextGain(SourceState& state) noexcept {
    return state.enabled.load(std::memory_order_acquire) == 0U
        ? 0.0F
        : state.targetGain.load();
}

} // namespace jamlink::audio
