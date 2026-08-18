// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/audio_topology.hpp"
#include "windows_audio_services.hpp"

#include <algorithm>
#include <memory>
#include <utility>

namespace jamlink::audio {
namespace {

class WindowsSoundcheckAudioService final : public ISoundcheckAudioService {
public:
    WindowsSoundcheckAudioService()
        : wasapi_(createWasapiSoundcheckAudioService()), asio_(createAsioSoundcheckAudioService()) {}

    ~WindowsSoundcheckAudioService() override { stop(); }

    [[nodiscard]] SoundcheckDeviceInventory enumerate() override {
        auto result = wasapi_->enumerate();
        auto asioInventory = asio_->enumerate();
        result.inputOptions.insert(
            result.inputOptions.end(),
            std::make_move_iterator(asioInventory.inputOptions.begin()),
            std::make_move_iterator(asioInventory.inputOptions.end()));
        result.outputOptions.insert(
            result.outputOptions.end(),
            std::make_move_iterator(asioInventory.outputOptions.begin()),
            std::make_move_iterator(asioInventory.outputOptions.end()));
        return result;
    }

    [[nodiscard]] bool start(const SoundcheckAudioConfiguration& configuration) override {
        stop();
        // Whether these three devices can run together is decided in one
        // place, and the same answer reaches the interface and the support
        // bundle. This used to be judged here from the output's backend alone,
        // which rejected a legitimate setup -- an interface for the guitar with
        // a USB microphone for voice -- as an unsupported format, when nothing
        // about any format was unsupported and the engine simply never started.
        const auto asEndpoint = [](const SoundcheckEndpointOption& option,
                                   SoundcheckBackend backend) {
            AudioTopologyEndpoint endpoint;
            endpoint.backend = backend;
            endpoint.endpointId = option.endpointId;
            endpoint.displayName = option.displayName;
            return endpoint;
        };
        AudioTopology topology;
        topology.instrument =
            asEndpoint(configuration.instrument, configuration.instrument.backend);
        topology.voice = asEndpoint(configuration.voice, configuration.voice.backend);
        topology.output = asEndpoint(configuration.output, configuration.output.backend);
        lastTopology_ = evaluateAudioTopology(topology);
        if (!lastTopology_.supported) {
            lastTelemetry_ = {};
            // Reported as a configuration problem rather than a device format
            // problem, because that is what it is and the difference decides
            // what a musician is told to do about it.
            lastTelemetry_.state = SoundcheckAudioState::UnsupportedFormat;
            lastTelemetry_.nativeError = -1;
            return false;
        }
        active_ = lastTopology_.masterBackend == SoundcheckBackend::Asio
            ? asio_.get() : wasapi_.get();
        active_->setPeerAudioExchange(peerExchange_);
        active_->setTunerEnabled(tunerEnabled_);
        active_->setMonitorControls(
            instrumentGain_, instrumentEnabled_, voiceGain_, voiceEnabled_);
        if (!active_->start(configuration)) {
            lastTelemetry_ = active_->telemetry();
            active_ = nullptr;
            return false;
        }
        return true;
    }

    [[nodiscard]] AudioTopologyResult audioTopology() const noexcept override {
        return lastTopology_;
    }

    void stop() noexcept override {
        if (active_ != nullptr) {
            active_->stop();
            lastTelemetry_ = active_->telemetry();
            active_ = nullptr;
        }
    }

    void setMonitorControls(
        float instrumentGain,
        bool instrumentEnabled,
        float voiceGain,
        bool voiceEnabled) noexcept override {
        instrumentGain_ = instrumentGain;
        voiceGain_ = voiceGain;
        instrumentEnabled_ = instrumentEnabled;
        voiceEnabled_ = voiceEnabled;
        if (active_ != nullptr) {
            active_->setMonitorControls(
                instrumentGain, instrumentEnabled, voiceGain, voiceEnabled);
        }
    }

    void requestOutputTest() noexcept override {
        if (active_ != nullptr) {
            active_->requestOutputTest();
        }
    }

    void setTunerEnabled(bool enabled) noexcept override {
        tunerEnabled_ = enabled;
        if (active_ != nullptr) {
            active_->setTunerEnabled(enabled);
        }
    }

    [[nodiscard]] TunerReading tunerReading() override {
        return active_ != nullptr ? active_->tunerReading() : TunerReading{};
    }

    [[nodiscard]] bool startRecording(
        const std::filesystem::path& directory,
        const std::string& sessionName) override {
        return active_ != nullptr && active_->startRecording(directory, sessionName);
    }

    void stopRecording() noexcept override {
        wasapi_->stopRecording();
        asio_->stopRecording();
    }

    [[nodiscard]] jamlink::record::RecorderTelemetry recorderTelemetry()
        const noexcept override {
        return active_ != nullptr
            ? active_->recorderTelemetry() : jamlink::record::RecorderTelemetry{};
    }

    void setPeerAudioExchange(
        jamlink::network::IPeerAudioExchange* exchange) noexcept override {
        peerExchange_ = exchange;
        if (active_ != nullptr) {
            active_->setPeerAudioExchange(exchange);
        }
    }

    [[nodiscard]] VoiceEndpointChangeResult tryReplaceVoiceEndpoint(
        const SoundcheckEndpointOption& option) override {
        return active_ != nullptr
            ? active_->tryReplaceVoiceEndpoint(option)
            : VoiceEndpointChangeResult::NotSupported;
    }

    void clearSignalHealth(SignalHealthPath path) noexcept override {
        if (active_ != nullptr) {
            active_->clearSignalHealth(path);
        }
    }

    void requestSignalHealthSelfTest(SignalHealthPath path) noexcept override {
        if (active_ != nullptr) {
            active_->requestSignalHealthSelfTest(path);
        }
    }

    [[nodiscard]] SoundcheckAudioTelemetry telemetry() const noexcept override {
        return active_ != nullptr ? active_->telemetry() : lastTelemetry_;
    }

private:
    std::unique_ptr<ISoundcheckAudioService> wasapi_;
    std::unique_ptr<ISoundcheckAudioService> asio_;
    ISoundcheckAudioService* active_{nullptr};
    AudioTopologyResult lastTopology_{};
    jamlink::network::IPeerAudioExchange* peerExchange_{nullptr};
    float instrumentGain_{1.0F};
    float voiceGain_{1.0F};
    bool instrumentEnabled_{true};
    bool voiceEnabled_{true};
    bool tunerEnabled_{false};
    SoundcheckAudioTelemetry lastTelemetry_{};
};

} // namespace

std::unique_ptr<ISoundcheckAudioService> createPlatformSoundcheckAudioService() {
    return std::make_unique<WindowsSoundcheckAudioService>();
}

} // namespace jamlink::audio
