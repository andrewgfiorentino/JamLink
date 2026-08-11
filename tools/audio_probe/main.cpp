// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/soundcheck_audio_service.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

int main() {
    auto service = jamlink::audio::createPlatformSoundcheckAudioService();
    const auto inventory = service->enumerate();
    const auto backendName = [](jamlink::audio::SoundcheckBackend backend) {
        return backend == jamlink::audio::SoundcheckBackend::Asio
            ? std::string_view("ASIO") : std::string_view("WASAPI");
    };
    std::cout << "Inputs:\n";
    for (const auto& input : inventory.inputOptions) {
        std::cout << "  [" << backendName(input.backend) << "] "
                  << input.displayName << "\n";
    }
    std::cout << "Outputs:\n";
    for (const auto& output : inventory.outputOptions) {
        std::cout << "  [" << backendName(output.backend) << "] "
                  << output.displayName << "\n";
    }

    const auto preferredAsioInput = std::find_if(
        inventory.inputOptions.begin(), inventory.inputOptions.end(), [](const auto& option) {
            return option.backend == jamlink::audio::SoundcheckBackend::Asio
                && option.backendId.find("Focusrite USB ASIO") != std::string::npos
                && option.primaryChannel == 1U;
        });
    const auto asioInput = preferredAsioInput != inventory.inputOptions.end()
        ? preferredAsioInput
        : std::find_if(
            inventory.inputOptions.begin(), inventory.inputOptions.end(), [](const auto& option) {
                return option.backend == jamlink::audio::SoundcheckBackend::Asio;
            });
    if (asioInput == inventory.inputOptions.end()) {
        std::cout << "No installed ASIO input was available; hybrid probe skipped.\n";
        return 0;
    }
    const auto asioOutput = std::find_if(
        inventory.outputOptions.begin(), inventory.outputOptions.end(),
        [&asioInput](const auto& option) {
            return option.backend == jamlink::audio::SoundcheckBackend::Asio
                && option.backendId == asioInput->backendId;
        });
    const auto separateWasapiVoice = std::find_if(
        inventory.inputOptions.begin(), inventory.inputOptions.end(), [](const auto& option) {
            return option.backend == jamlink::audio::SoundcheckBackend::WasapiShared
                && option.displayName.find("Focusrite") == std::string::npos;
        });
    const auto wasapiVoice = separateWasapiVoice != inventory.inputOptions.end()
        ? separateWasapiVoice
        : std::find_if(
            inventory.inputOptions.begin(), inventory.inputOptions.end(), [](const auto& option) {
                return option.backend == jamlink::audio::SoundcheckBackend::WasapiShared;
            });
    if (asioOutput == inventory.outputOptions.end()
        || wasapiVoice == inventory.inputOptions.end()) {
        std::cout << "A complete ASIO-master/WASAPI-voice combination was unavailable.\n";
        return 0;
    }
    const auto practicalBuffer = std::find_if(
        asioOutput->bufferFrameOptions.begin(), asioOutput->bufferFrameOptions.end(),
        [](std::uint32_t frames) { return frames >= 64U; });
    const std::uint32_t buffer = practicalBuffer != asioOutput->bufferFrameOptions.end()
        ? *practicalBuffer
        : asioOutput->bufferFrameOptions.empty() ? 0U
                                                 : asioOutput->bufferFrameOptions.back();
    const jamlink::audio::SoundcheckAudioConfiguration configuration{
        *asioInput, *wasapiVoice, *asioOutput, buffer,
        0.0F, 0.0F, false, false};
    const bool started = service->start(configuration);
    auto telemetry = service->telemetry();
    std::cout << "Hybrid selection:\n  instrument: " << asioInput->displayName
              << "\n  voice: " << wasapiVoice->displayName
              << "\n  output: " << asioOutput->displayName << '\n';
    if (!started) {
        std::cout << "Hybrid start failed safely; state="
                  << static_cast<int>(telemetry.state)
                  << " native=" << telemetry.nativeError
                  << " secondary=" << telemetry.secondaryVoiceNativeError << '\n';
        return 1;
    }
    for (std::size_t check = 0U; check < 15U; ++check) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        telemetry = service->telemetry();
        if (telemetry.state != jamlink::audio::SoundcheckAudioState::Running) {
            break;
        }
    }
    const auto replacementVoice = std::find_if(
        inventory.inputOptions.begin(), inventory.inputOptions.end(),
        [&wasapiVoice](const auto& option) {
            return option.backend == jamlink::audio::SoundcheckBackend::WasapiShared
                && option.endpointId != wasapiVoice->endpointId
                && option.displayName.find("Focusrite") == std::string::npos;
        });
    if (telemetry.state == jamlink::audio::SoundcheckAudioState::Running
        && replacementVoice != inventory.inputOptions.end()) {
        const auto replacementResult = service->tryReplaceVoiceEndpoint(*replacementVoice);
        std::cout << "Secondary microphone replacement result="
                  << static_cast<int>(replacementResult) << " using "
                  << replacementVoice->displayName << '\n';
        if (replacementResult != jamlink::audio::VoiceEndpointChangeResult::Applied) {
            service->stop();
            return 1;
        }
    }
    for (std::size_t check = 0U; check < 15U; ++check) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        telemetry = service->telemetry();
        if (telemetry.state != jamlink::audio::SoundcheckAudioState::Running) {
            break;
        }
    }
    service->stop();
    std::cout << "Hybrid probe state=" << static_cast<int>(telemetry.state)
              << " secondary_active=" << telemetry.secondaryVoiceActive
              << " underruns=" << telemetry.underruns
              << " overruns=" << telemetry.overruns << '\n';
    return telemetry.state == jamlink::audio::SoundcheckAudioState::Running
            && telemetry.secondaryVoiceActive ? 0 : 1;
}
