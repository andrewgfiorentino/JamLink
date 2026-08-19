// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/audio_topology.hpp"
#include "jamlink/audio/soundcheck_backend.hpp"

#include "jamlink/audio/instrument_tuner.hpp"
#include "jamlink/audio/level_meter.hpp"
#include "jamlink/record/session_recorder.hpp"

#include <cstdint>
#include <filesystem>
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
    // What the driver itself recommends, which is what every DAW opens at.
    // Zero when the device does not say. Distinct from the smallest size it
    // will admit to: that is a floor, not an operating point, and starting
    // there produces drop-outs on a healthy machine doing nothing.
    std::uint32_t preferredBufferFrames{0U};
    SoundcheckBackend backend{SoundcheckBackend::WasapiShared};
    // ASIO uses the driver name here. WASAPI leaves it empty and identifies
    // the endpoint entirely with endpointId.
    std::string backendId;
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

struct SignalHealthTelemetry final {
    float currentPeak{0.0F};
    float currentRms{0.0F};
    float peakHold{0.0F};
    bool clipped{false};
    bool nearFullScaleRisk{false};
    bool diagnosticClip{false};
    std::uint64_t clipSamples{0U};
    std::uint64_t clipEvents{0U};
    std::uint64_t latestClipSampleOffset{0U};
    std::uint64_t invalidSamples{0U};
};

[[nodiscard]] inline SignalHealthTelemetry signalHealthTelemetry(
    const LevelSnapshot& snapshot) noexcept {
    return SignalHealthTelemetry{
        snapshot.peakLinear,
        snapshot.rmsLinear,
        snapshot.peakHoldLinear,
        snapshot.clipped,
        snapshot.nearFullScaleRisk,
        snapshot.diagnosticClip,
        snapshot.clipSampleCount,
        snapshot.clipEventCount,
        snapshot.latestClipSampleOffset,
        snapshot.invalidSampleCount};
}

enum class SignalHealthPath : std::uint8_t {
    InstrumentInput,
    VoiceInput,
    InstrumentSend,
    VoiceSend,
    MonitorMix,
    RecordingInstrument,
    RecordingVoice
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
    bool secondaryVoiceActive{true};
    std::int32_t secondaryVoiceNativeError{0};
    SignalHealthTelemetry instrumentInput;
    SignalHealthTelemetry voiceInput;
    SignalHealthTelemetry instrumentSend;
    SignalHealthTelemetry voiceSend;
    SignalHealthTelemetry monitorMix;
    SignalHealthTelemetry recordingInstrument;
    SignalHealthTelemetry recordingVoice;
};

enum class VoiceEndpointChangeResult : std::uint8_t {
    NotSupported,
    Applied,
    Failed
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

    // Control-thread only. Recording taps the same points the mixer uses and
    // hands frames to a disk worker; the callback never touches the filesystem.
    [[nodiscard]] virtual bool startRecording(
        const std::filesystem::path& directory,
        const std::string& sessionName) = 0;
    virtual void stopRecording() noexcept = 0;
    // Which tracks the next take will contain. Applies from the next start:
    // changing what a take holds while it is being written would leave a file
    // that stops partway through and looks exactly like a failure.
    virtual void setRecordTrackSelection(
        const jamlink::record::RecordTrackSelection&) noexcept {}
    [[nodiscard]] virtual jamlink::record::RecorderTelemetry recorderTelemetry()
        const noexcept = 0;
    // Control-thread only; processing must be stopped while changing this
    // pointer. A null exchange is the enforced Private Soundcheck state.
    virtual void setPeerAudioExchange(
        jamlink::network::IPeerAudioExchange* exchange) noexcept = 0;
    // Allows a secondary independently clocked voice capture endpoint to be
    // replaced without stopping the master instrument/output stream.
    [[nodiscard]] virtual VoiceEndpointChangeResult tryReplaceVoiceEndpoint(
        const SoundcheckEndpointOption&) {
        return VoiceEndpointChangeResult::NotSupported;
    }
    // Control-thread request. The owning audio callback consumes the reset;
    // no control-thread lock or direct mutation of callback state is required.
    virtual void clearSignalHealth(SignalHealthPath) noexcept {}
    // Schedules a silent detector/latch/reset self-test in the owning audio
    // callback. No test sample may reach monitor, recording, or network buses.
    virtual void requestSignalHealthSelfTest(SignalHealthPath) noexcept {}
    [[nodiscard]] virtual SoundcheckAudioTelemetry telemetry() const noexcept = 0;

    // Whether the chosen devices can run together, and the one thing to change
    // if not. Defaulted so only the dispatcher, which owns the decision, has to
    // answer it. The interface and the support bundle both read this rather
    // than each re-deriving the rules.
    [[nodiscard]] virtual AudioTopologyResult audioTopology() const noexcept {
        return {};
    }
};

[[nodiscard]] std::unique_ptr<ISoundcheckAudioService>
createPlatformSoundcheckAudioService();

} // namespace jamlink::audio
