// SPDX-License-Identifier: GPL-3.0-or-later

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
        const auto backend = configuration.output.backend;
        const bool validWasapi = backend == SoundcheckBackend::WasapiShared
            && configuration.instrument.backend == SoundcheckBackend::WasapiShared
            && configuration.voice.backend == SoundcheckBackend::WasapiShared;
        const bool validAsioMaster = backend == SoundcheckBackend::Asio
            && configuration.instrument.backend == SoundcheckBackend::Asio;
        if (!validWasapi && !validAsioMaster) {
            lastTelemetry_ = {};
            lastTelemetry_.state = SoundcheckAudioState::UnsupportedFormat;
            lastTelemetry_.nativeError = -1;
            return false;
        }
        active_ = backend == SoundcheckBackend::Asio ? asio_.get() : wasapi_.get();
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

    [[nodiscard]] SoundcheckAudioTelemetry telemetry() const noexcept override {
        return active_ != nullptr ? active_->telemetry() : lastTelemetry_;
    }

private:
    std::unique_ptr<ISoundcheckAudioService> wasapi_;
    std::unique_ptr<ISoundcheckAudioService> asio_;
    ISoundcheckAudioService* active_{nullptr};
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
