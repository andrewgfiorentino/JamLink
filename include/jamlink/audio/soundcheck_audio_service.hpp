// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/instrument_tuner.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jamlink::network {
class IPeerAudioExchange;
}

namespace jamlink::audio {

struct SoundcheckEndpointOption final {
    std::string endpointId;
    std::string displayName;
    std::uint32_t primaryChannel{0U};
    std::uint32_t secondaryChannel{0U};
    bool hasSecondaryChannel{false};
    std::uint32_t mixSampleRate{0U};
    std::vector<std::uint32_t> bufferFrameOptions;
};

struct SoundcheckDeviceInventory final {
    std::vector<SoundcheckEndpointOption> inputOptions;
    std::vector<SoundcheckEndpointOption> outputOptions;
};

struct SoundcheckAudioConfiguration final {
    SoundcheckEndpointOption instrument;
    SoundcheckEndpointOption voice;
    SoundcheckEndpointOption output;
    std::uint32_t requestedBufferFrames{0U};
    float instrumentGain{1.0F};
    float voiceGain{1.0F};
    bool instrumentEnabled{true};
    bool voiceEnabled{true};
};

enum class SoundcheckAudioState : std::uint8_t {
    Stopped,
    Starting,
    Running,
    NoEndpoints,
    DeviceUnavailable,
    UnsupportedFormat,
    InitializationFailed,
    DeviceInvalidated
};

struct SoundcheckAudioTelemetry final {
    SoundcheckAudioState state{SoundcheckAudioState::Stopped};
    float instrumentPeak{0.0F};
    float voicePeak{0.0F};
    float outputPeak{0.0F};
    std::uint32_t outputSampleRate{0U};
    std::uint32_t outputBufferFrames{0U};
    std::uint64_t underruns{0U};
    std::uint64_t overruns{0U};
    bool rawMode{false};
    std::int32_t nativeError{0};
};

// Qt-free control boundary for the private local monitor. Platform APIs remain
// behind this interface, and no network or transport object is reachable here.
class ISoundcheckAudioService {
public:
    virtual ~ISoundcheckAudioService() = default;

    [[nodiscard]] virtual SoundcheckDeviceInventory enumerate() = 0;
    [[nodiscard]] virtual bool start(const SoundcheckAudioConfiguration& configuration) = 0;
    virtual void stop() noexcept = 0;
    virtual void setMonitorControls(
        float instrumentGain,
        bool instrumentEnabled,
        float voiceGain,
        bool voiceEnabled) noexcept = 0;
    virtual void requestOutputTest() noexcept = 0;
    // Taps the instrument input for pitch detection. The tap is a copy: the
    // monitored signal path and its latency are unchanged either way.
    virtual void setTunerEnabled(bool enabled) noexcept = 0;
    // Control-thread only. Analyses the most recent instrument window.
    [[nodiscard]] virtual TunerReading tunerReading() = 0;
    // Control-thread only; processing must be stopped while changing this
    // pointer. A null exchange is the enforced Private Soundcheck state.
    virtual void setPeerAudioExchange(
        jamlink::network::IPeerAudioExchange* exchange) noexcept = 0;
    [[nodiscard]] virtual SoundcheckAudioTelemetry telemetry() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<ISoundcheckAudioService>
createPlatformSoundcheckAudioService();

} // namespace jamlink::audio
