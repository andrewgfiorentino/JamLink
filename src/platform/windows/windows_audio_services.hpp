// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/soundcheck_audio_service.hpp"
#include "jamlink/audio/hybrid_clock_bridge.hpp"

#include <memory>

namespace jamlink::audio {

struct SecondaryCaptureTelemetry final {
    SoundcheckAudioState state{SoundcheckAudioState::Stopped};
    std::uint32_t sampleRate{0U};
    float peak{0.0F};
    std::int32_t nativeError{0};
    SignalHealthTelemetry inputHealth;
};

class ISecondaryWasapiCapture {
public:
    virtual ~ISecondaryWasapiCapture() = default;
    [[nodiscard]] virtual bool prepare(
        const SoundcheckEndpointOption& option,
        HybridClockBridge& destination) = 0;
    [[nodiscard]] virtual bool begin() noexcept = 0;
    virtual void stop() noexcept = 0;
    virtual void clearSignalHealth() noexcept = 0;
    virtual void requestSignalHealthSelfTest() noexcept = 0;
    [[nodiscard]] virtual bool inputClipped() const noexcept = 0;
    [[nodiscard]] virtual SecondaryCaptureTelemetry telemetry() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ISoundcheckAudioService>
createWasapiSoundcheckAudioService();

[[nodiscard]] std::unique_ptr<ISoundcheckAudioService>
createAsioSoundcheckAudioService();

[[nodiscard]] std::unique_ptr<ISecondaryWasapiCapture>
createSecondaryWasapiCapture();

} // namespace jamlink::audio
