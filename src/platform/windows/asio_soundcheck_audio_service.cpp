// SPDX-License-Identifier: GPL-3.0-or-later

#define NOMINMAX
#include <Windows.h>

#include "windows_audio_services.hpp"

#include "jamlink/audio/native_sample_conversion.hpp"
#include "jamlink/audio/realtime_atomic.hpp"
#include "jamlink/network/peer_audio_transport.hpp"

#include "asiosys.h"
#include "asio.h"
#include "asiodrivers.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

extern AsioDrivers* asioDrivers;
bool loadAsioDriver(char* name);

namespace jamlink::audio {
namespace {

constexpr std::uint32_t asioSampleRate = 48'000U;
constexpr long maximumAsioChannels = 512L;
constexpr long maximumAsioBufferFrames = 16'384L;
constexpr float outputCeiling = 0.98F;

[[nodiscard]] bool asioFormat(ASIOSampleType type, NativeSampleFormat& result) noexcept {
    switch (type) {
    case ASIOSTInt16LSB:
        result = NativeSampleFormat::Int16LittleEndian;
        return true;
    case ASIOSTInt24LSB:
        result = NativeSampleFormat::Int24LittleEndian;
        return true;
    case ASIOSTInt32LSB:
        result = NativeSampleFormat::Int32LittleEndian;
        return true;
    case ASIOSTFloat32LSB:
        result = NativeSampleFormat::Float32LittleEndian;
        return true;
    case ASIOSTFloat64LSB:
        result = NativeSampleFormat::Float64LittleEndian;
        return true;
    case ASIOSTInt16MSB:
        result = NativeSampleFormat::Int16BigEndian;
        return true;
    case ASIOSTInt24MSB:
        result = NativeSampleFormat::Int24BigEndian;
        return true;
    case ASIOSTInt32MSB:
        result = NativeSampleFormat::Int32BigEndian;
        return true;
    case ASIOSTFloat32MSB:
        result = NativeSampleFormat::Float32BigEndian;
        return true;
    case ASIOSTFloat64MSB:
        result = NativeSampleFormat::Float64BigEndian;
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::string channelName(const ASIOChannelInfo& info) {
    const std::size_t length = strnlen_s(info.name, std::size(info.name));
    return length == 0U ? std::string("Channel ") + std::to_string(info.channel + 1L)
                        : std::string(info.name, length);
}

[[nodiscard]] std::vector<std::uint32_t> bufferOptions(
    long minimum,
    long maximum,
    long preferred,
    long granularity) {
    std::vector<std::uint32_t> result;
    if (minimum <= 0L || maximum < minimum || maximum > maximumAsioBufferFrames) {
        return result;
    }
    const auto add = [&result, minimum, maximum](long value) {
        if (value >= minimum && value <= maximum) {
            result.push_back(static_cast<std::uint32_t>(value));
        }
    };
    add(preferred);
    add(minimum);
    if (granularity == -1L) {
        for (long value = minimum; value <= maximum && result.size() < 32U;) {
            add(value);
            if (value > maximum / 2L) {
                break;
            }
            value *= 2L;
        }
    } else if (granularity > 0L) {
        for (long value = minimum; value <= maximum && result.size() < 32U;
             value += granularity) {
            add(value);
            if (value > maximum - granularity) {
                break;
            }
        }
    } else {
        add(maximum);
    }
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

class ScopedAsioDriver final {
public:
    ScopedAsioDriver() = default;
    ~ScopedAsioDriver() { reset(); }
    ScopedAsioDriver(const ScopedAsioDriver&) = delete;
    ScopedAsioDriver& operator=(const ScopedAsioDriver&) = delete;

    [[nodiscard]] bool open(const std::string& driverName) {
        reset();
        std::array<char, MAXDRVNAMELEN> mutableName{};
        if (driverName.size() >= mutableName.size()) {
            return false;
        }
        std::memcpy(mutableName.data(), driverName.data(), driverName.size());
        if (!loadAsioDriver(mutableName.data())) {
            return false;
        }
        loaded_ = true;
        ASIODriverInfo info{};
        info.asioVersion = 2L;
        info.sysRef = GetDesktopWindow();
        if (ASIOInit(&info) != ASE_OK) {
            reset();
            return false;
        }
        initialized_ = true;
        return true;
    }

    void reset() noexcept {
        if (initialized_) {
            static_cast<void>(ASIOExit());
        } else if (loaded_ && asioDrivers != nullptr) {
            asioDrivers->removeCurrentDriver();
        }
        initialized_ = false;
        loaded_ = false;
    }

private:
    bool loaded_{false};
    bool initialized_{false};
};

[[nodiscard]] std::vector<std::string> installedAsioDrivers() {
    if (asioDrivers == nullptr) {
        asioDrivers = new AsioDrivers();
    }
    std::array<std::array<char, MAXDRVNAMELEN>, 64U> storage{};
    std::array<char*, 64U> names{};
    for (std::size_t index = 0U; index < names.size(); ++index) {
        names[index] = storage[index].data();
    }
    const long count = asioDrivers->getDriverNames(names.data(), static_cast<long>(names.size()));
    std::vector<std::string> result;
    result.reserve(count > 0L ? static_cast<std::size_t>(count) : 0U);
    for (long index = 0L; index < count; ++index) {
        const std::size_t length = strnlen_s(
            storage[static_cast<std::size_t>(index)].data(), MAXDRVNAMELEN);
        if (length > 0U) {
            result.emplace_back(storage[static_cast<std::size_t>(index)].data(), length);
        }
    }
    return result;
}

[[nodiscard]] SoundcheckDeviceInventory enumerateAsioEndpoints() {
    SoundcheckDeviceInventory inventory;
    for (const auto& driverName : installedAsioDrivers()) {
        ScopedAsioDriver driver;
        if (!driver.open(driverName) || ASIOCanSampleRate(asioSampleRate) != ASE_OK) {
            continue;
        }
        long inputs = 0L;
        long outputs = 0L;
        long minimum = 0L;
        long maximum = 0L;
        long preferred = 0L;
        long granularity = 0L;
        if (ASIOGetChannels(&inputs, &outputs) != ASE_OK
            || ASIOGetBufferSize(&minimum, &maximum, &preferred, &granularity) != ASE_OK
            || inputs < 0L || outputs < 0L
            || inputs > maximumAsioChannels || outputs > maximumAsioChannels) {
            continue;
        }
        const auto buffers = bufferOptions(minimum, maximum, preferred, granularity);
        if (buffers.empty()) {
            continue;
        }
        const std::string endpointId = "asio:" + driverName;
        for (long channel = 0L; channel < inputs; ++channel) {
            ASIOChannelInfo info{};
            info.channel = channel;
            info.isInput = ASIOTrue;
            NativeSampleFormat format{};
            if (ASIOGetChannelInfo(&info) != ASE_OK || !asioFormat(info.type, format)) {
                continue;
            }
            inventory.inputOptions.push_back(SoundcheckEndpointOption{
                endpointId,
                driverName + " · " + channelName(info),
                static_cast<std::uint32_t>(channel),
                0U,
                false,
                asioSampleRate,
                buffers,
                SoundcheckBackend::Asio,
                driverName});
        }
        for (long channel = 0L; channel < outputs; channel += 2L) {
            ASIOChannelInfo first{};
            first.channel = channel;
            first.isInput = ASIOFalse;
            NativeSampleFormat firstFormat{};
            if (ASIOGetChannelInfo(&first) != ASE_OK || !asioFormat(first.type, firstFormat)) {
                continue;
            }
            const bool hasSecond = channel + 1L < outputs;
            bool validSecond = false;
            if (hasSecond) {
                ASIOChannelInfo second{};
                second.channel = channel + 1L;
                second.isInput = ASIOFalse;
                NativeSampleFormat secondFormat{};
                validSecond = ASIOGetChannelInfo(&second) == ASE_OK
                    && asioFormat(second.type, secondFormat);
            }
            inventory.outputOptions.push_back(SoundcheckEndpointOption{
                endpointId,
                driverName + " · Output" + (validSecond ? "s " : " ")
                    + std::to_string(channel + 1L)
                    + (validSecond ? "–" + std::to_string(channel + 2L) : ""),
                static_cast<std::uint32_t>(channel),
                validSecond ? static_cast<std::uint32_t>(channel + 1L)
                            : static_cast<std::uint32_t>(channel),
                validSecond,
                asioSampleRate,
                buffers,
                SoundcheckBackend::Asio,
                driverName});
        }
    }
    return inventory;
}

class AsioSoundcheckAudioService final : public ISoundcheckAudioService {
public:
    AsioSoundcheckAudioService()
        : secondaryVoice_(createSecondaryWasapiCapture()),
          voiceBridge_(65'536U, 16'384U, asioSampleRate) {}
    ~AsioSoundcheckAudioService() override { stop(); }
    AsioSoundcheckAudioService(const AsioSoundcheckAudioService&) = delete;
    AsioSoundcheckAudioService& operator=(const AsioSoundcheckAudioService&) = delete;

    [[nodiscard]] SoundcheckDeviceInventory enumerate() override {
        stop();
        return enumerateAsioEndpoints();
    }

    [[nodiscard]] bool start(const SoundcheckAudioConfiguration& configuration) override {
        stop();
        state_.store(SoundcheckAudioState::Starting, std::memory_order_release);
        if (configuration.instrument.backend != SoundcheckBackend::Asio
            || configuration.output.backend != SoundcheckBackend::Asio
            || configuration.instrument.backendId.empty()
            || configuration.instrument.backendId != configuration.output.backendId
            || (configuration.voice.backend == SoundcheckBackend::Asio
                && configuration.instrument.backendId != configuration.voice.backendId)) {
            state_.store(SoundcheckAudioState::UnsupportedFormat, std::memory_order_release);
            return false;
        }
        hybridVoice_ = configuration.voice.backend == SoundcheckBackend::WasapiShared;
        if (!driver_.open(configuration.output.backendId)) {
            state_.store(SoundcheckAudioState::DeviceUnavailable, std::memory_order_release);
            nativeError_.store(ASE_NotPresent, std::memory_order_relaxed);
            return false;
        }
        if (!configure(configuration)) {
            cleanup();
            return false;
        }
        setMonitorControls(
            configuration.instrumentGain,
            configuration.instrumentEnabled,
            configuration.voiceGain,
            configuration.voiceEnabled);
        active_.store(this, std::memory_order_release);
        if (ASIOStart() != ASE_OK) {
            active_.store(nullptr, std::memory_order_release);
            state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
            cleanup();
            return false;
        }
        started_ = true;
        if (hybridVoice_ && !secondaryVoice_->begin()) {
            static_cast<void>(ASIOStop());
            started_ = false;
            active_.store(nullptr, std::memory_order_release);
            const auto secondary = secondaryVoice_->telemetry();
            state_.store(secondary.state, std::memory_order_release);
            nativeError_.store(secondary.nativeError, std::memory_order_relaxed);
            cleanup();
            return false;
        }
        state_.store(SoundcheckAudioState::Running, std::memory_order_release);
        return true;
    }

    void stop() noexcept override {
        if (started_) {
            static_cast<void>(ASIOStop());
            started_ = false;
        }
        active_.store(nullptr, std::memory_order_release);
        cleanup();
        recorder_.stop();
        if (state_.load(std::memory_order_acquire) == SoundcheckAudioState::Running
            || state_.load(std::memory_order_acquire) == SoundcheckAudioState::Starting) {
            state_.store(SoundcheckAudioState::Stopped, std::memory_order_release);
        }
    }

    void setMonitorControls(
        float instrumentGain,
        bool instrumentEnabled,
        float voiceGain,
        bool voiceEnabled) noexcept override {
        instrumentGain_.store(std::clamp(instrumentGain, 0.0F, 1.0F));
        voiceGain_.store(std::clamp(voiceGain, 0.0F, 1.0F));
        instrumentEnabled_.store(instrumentEnabled ? 1U : 0U, std::memory_order_release);
        voiceEnabled_.store(voiceEnabled ? 1U : 0U, std::memory_order_release);
    }

    void requestOutputTest() noexcept override {
        testToneRequest_.fetch_add(1U, std::memory_order_release);
    }

    void setTunerEnabled(bool enabled) noexcept override {
        tunerEnabled_.store(enabled ? 1U : 0U, std::memory_order_release);
    }

    [[nodiscard]] TunerReading tunerReading() override {
        return tunerEnabled_.load(std::memory_order_acquire) != 0U
            ? tuner_.analyse() : TunerReading{};
    }

    [[nodiscard]] bool startRecording(
        const std::filesystem::path& directory,
        const std::string& sessionName) override {
        return started_ && recorder_.start(directory, sessionName, asioSampleRate);
    }

    void stopRecording() noexcept override { recorder_.stop(); }

    [[nodiscard]] jamlink::record::RecorderTelemetry recorderTelemetry()
        const noexcept override {
        return recorder_.telemetry();
    }

    void setPeerAudioExchange(
        jamlink::network::IPeerAudioExchange* exchange) noexcept override {
        peerExchange_ = exchange;
    }

    [[nodiscard]] VoiceEndpointChangeResult tryReplaceVoiceEndpoint(
        const SoundcheckEndpointOption& option) override {
        if (!started_ || !hybridVoice_ || option.backend != SoundcheckBackend::WasapiShared) {
            return VoiceEndpointChangeResult::NotSupported;
        }
        secondaryVoice_->stop();
        if (!secondaryVoice_->prepare(option, voiceBridge_)) {
            return VoiceEndpointChangeResult::Failed;
        }
        const auto prepared = secondaryVoice_->telemetry();
        const std::uint32_t transition = voiceBridge_.requestSourceTransition(prepared.sampleRate);
        for (std::size_t attempt = 0U;
             attempt < 1'000U && !voiceBridge_.transitionApplied(transition); ++attempt) {
            Sleep(1U);
        }
        if (!voiceBridge_.transitionApplied(transition) || !secondaryVoice_->begin()) {
            secondaryVoice_->stop();
            return VoiceEndpointChangeResult::Failed;
        }
        return VoiceEndpointChangeResult::Applied;
    }

    [[nodiscard]] SoundcheckAudioTelemetry telemetry() const noexcept override {
        const auto secondary = secondaryVoice_->telemetry();
        return SoundcheckAudioTelemetry{
            state_.load(std::memory_order_acquire),
            instrumentPeak_.load(),
            voicePeak_.load(),
            outputPeak_.load(),
            asioSampleRate,
            bufferFrames_ > 0L ? static_cast<std::uint32_t>(bufferFrames_) : 0U,
            underruns_.load(std::memory_order_relaxed)
                + (hybridVoice_ ? voiceBridge_.underrunCount() : 0U),
            overruns_.load(std::memory_order_relaxed)
                + (hybridVoice_ ? voiceBridge_.overrunCount() : 0U),
            false,
            nativeError_.load(std::memory_order_relaxed),
            !hybridVoice_ || secondary.state == SoundcheckAudioState::Running,
            hybridVoice_ ? secondary.nativeError : 0};
    }

private:
    [[nodiscard]] bool fail(ASIOError error, SoundcheckAudioState state) noexcept {
        nativeError_.store(static_cast<std::int32_t>(error), std::memory_order_relaxed);
        state_.store(state, std::memory_order_release);
        return false;
    }

    [[nodiscard]] bool addBuffer(
        bool input,
        std::uint32_t channel,
        std::size_t& destinationIndex) noexcept {
        for (std::size_t index = 0U; index < bufferCount_; ++index) {
            if ((buffers_[index].isInput == ASIOTrue) == input
                && buffers_[index].channelNum == static_cast<long>(channel)) {
                destinationIndex = index;
                return true;
            }
        }
        if (bufferCount_ >= buffers_.size()) {
            return false;
        }
        destinationIndex = bufferCount_++;
        buffers_[destinationIndex] = ASIOBufferInfo{
            input ? ASIOTrue : ASIOFalse, static_cast<long>(channel), {nullptr, nullptr}};
        return true;
    }

    [[nodiscard]] bool configure(const SoundcheckAudioConfiguration& configuration) {
        long inputChannels = 0L;
        long outputChannels = 0L;
        long minimum = 0L;
        long maximum = 0L;
        long preferred = 0L;
        long granularity = 0L;
        ASIOError error = ASIOGetChannels(&inputChannels, &outputChannels);
        if (error != ASE_OK) {
            return fail(error, SoundcheckAudioState::InitializationFailed);
        }
        error = ASIOGetBufferSize(&minimum, &maximum, &preferred, &granularity);
        if (error != ASE_OK) {
            return fail(error, SoundcheckAudioState::InitializationFailed);
        }
        if (configuration.instrument.primaryChannel >= static_cast<std::uint32_t>(inputChannels)
            || (!hybridVoice_
                && configuration.voice.primaryChannel >= static_cast<std::uint32_t>(inputChannels))
            || configuration.output.primaryChannel >= static_cast<std::uint32_t>(outputChannels)
            || (configuration.output.hasSecondaryChannel
                && configuration.output.secondaryChannel
                    >= static_cast<std::uint32_t>(outputChannels))) {
            return fail(ASE_InvalidParameter, SoundcheckAudioState::UnsupportedFormat);
        }
        if (ASIOCanSampleRate(asioSampleRate) != ASE_OK) {
            return fail(ASE_NoClock, SoundcheckAudioState::UnsupportedFormat);
        }
        ASIOSampleRate currentRate = 0.0;
        error = ASIOGetSampleRate(&currentRate);
        if (error != ASE_OK || std::abs(currentRate - asioSampleRate) > 0.5) {
            error = ASIOSetSampleRate(asioSampleRate);
            if (error != ASE_OK) {
                return fail(error, SoundcheckAudioState::UnsupportedFormat);
            }
        }
        const long requested = configuration.requestedBufferFrames == 0U
            ? preferred : static_cast<long>(configuration.requestedBufferFrames);
        const auto options = bufferOptions(minimum, maximum, preferred, granularity);
        if (std::find(options.begin(), options.end(), static_cast<std::uint32_t>(requested))
            == options.end()) {
            return fail(ASE_InvalidMode, SoundcheckAudioState::UnsupportedFormat);
        }
        bufferFrames_ = requested;
        if (!addBuffer(true, configuration.instrument.primaryChannel, instrumentBuffer_)
            || !addBuffer(false, configuration.output.primaryChannel, outputLeftBuffer_)) {
            return fail(ASE_InvalidParameter, SoundcheckAudioState::UnsupportedFormat);
        }
        if (!hybridVoice_
            && !addBuffer(true, configuration.voice.primaryChannel, voiceBuffer_)) {
            return fail(ASE_InvalidParameter, SoundcheckAudioState::UnsupportedFormat);
        }
        outputRightBuffer_ = outputLeftBuffer_;
        if (configuration.output.hasSecondaryChannel
            && configuration.output.secondaryChannel != configuration.output.primaryChannel
            && !addBuffer(false, configuration.output.secondaryChannel, outputRightBuffer_)) {
            return fail(ASE_InvalidParameter, SoundcheckAudioState::UnsupportedFormat);
        }

        callbacks_ = ASIOCallbacks{
            &callbackBufferSwitch,
            &callbackSampleRateChanged,
            &callbackMessage,
            &callbackBufferSwitchTimeInfo};
        active_.store(this, std::memory_order_release);
        error = ASIOCreateBuffers(
            buffers_.data(), static_cast<long>(bufferCount_), bufferFrames_, &callbacks_);
        if (error != ASE_OK) {
            active_.store(nullptr, std::memory_order_release);
            return fail(error, SoundcheckAudioState::InitializationFailed);
        }
        buffersCreated_ = true;
        for (std::size_t index = 0U; index < bufferCount_; ++index) {
            ASIOChannelInfo info{};
            info.channel = buffers_[index].channelNum;
            info.isInput = buffers_[index].isInput;
            error = ASIOGetChannelInfo(&info);
            if (error != ASE_OK || !asioFormat(info.type, formats_[index])) {
                active_.store(nullptr, std::memory_order_release);
                return fail(
                    error == ASE_OK ? ASE_InvalidMode : error,
                    SoundcheckAudioState::UnsupportedFormat);
            }
        }
        long inputLatency = 0L;
        long outputLatency = 0L;
        static_cast<void>(ASIOGetLatencies(&inputLatency, &outputLatency));
        instrumentScratch_.assign(static_cast<std::size_t>(bufferFrames_), 0.0F);
        voiceScratch_.assign(static_cast<std::size_t>(bufferFrames_), 0.0F);
        remoteScratch_.assign(static_cast<std::size_t>(bufferFrames_), 0.0F);
        remoteStreamScratch_.assign(static_cast<std::size_t>(bufferFrames_), 0.0F);
        mixScratch_.assign(static_cast<std::size_t>(bufferFrames_), 0.0F);
        for (const std::size_t outputIndex : std::array{outputLeftBuffer_, outputRightBuffer_}) {
            for (std::size_t half = 0U; half < 2U; ++half) {
                floatToNativeSamples(
                    formats_[outputIndex],
                    std::span<const float>(mixScratch_.data(), mixScratch_.size()),
                    buffers_[outputIndex].buffers[half]);
            }
        }
        instrumentPeak_.store(0.0F);
        voicePeak_.store(0.0F);
        outputPeak_.store(0.0F);
        underruns_.store(0U, std::memory_order_relaxed);
        overruns_.store(0U, std::memory_order_relaxed);
        nativeError_.store(0, std::memory_order_relaxed);
        currentInstrumentGain_ = instrumentGain_.load();
        currentVoiceGain_ = voiceGain_.load();
        if (hybridVoice_) {
            if (!secondaryVoice_->prepare(configuration.voice, voiceBridge_)) {
                const auto secondary = secondaryVoice_->telemetry();
                nativeError_.store(secondary.nativeError, std::memory_order_relaxed);
                state_.store(secondary.state, std::memory_order_release);
                return false;
            }
            voiceBridge_.configureStopped(secondaryVoice_->telemetry().sampleRate);
        }
        return true;
    }

    void cleanup() noexcept {
        secondaryVoice_->stop();
        if (buffersCreated_) {
            static_cast<void>(ASIODisposeBuffers());
            buffersCreated_ = false;
        }
        driver_.reset();
        bufferCount_ = 0U;
        bufferFrames_ = 0L;
    }

    void process(long doubleBufferIndex) noexcept {
        if (doubleBufferIndex < 0L || doubleBufferIndex > 1L || bufferFrames_ <= 0L) {
            overruns_.fetch_add(1U, std::memory_order_relaxed);
            return;
        }
        const auto half = static_cast<std::size_t>(doubleBufferIndex);
        const std::size_t frames = static_cast<std::size_t>(bufferFrames_);
        nativeSamplesToFloat(
            formats_[instrumentBuffer_], buffers_[instrumentBuffer_].buffers[half],
            std::span<float>(instrumentScratch_.data(), frames));
        if (hybridVoice_) {
            static_cast<void>(voiceBridge_.pull(
                std::span<float>(voiceScratch_.data(), frames)));
        } else {
            nativeSamplesToFloat(
                formats_[voiceBuffer_], buffers_[voiceBuffer_].buffers[half],
                std::span<float>(voiceScratch_.data(), frames));
        }

        float instrumentPeak = 0.0F;
        float voicePeak = 0.0F;
        for (std::size_t frame = 0U; frame < frames; ++frame) {
            instrumentPeak = std::max(instrumentPeak, std::abs(instrumentScratch_[frame]));
            voicePeak = std::max(voicePeak, std::abs(voiceScratch_[frame]));
        }
        instrumentPeak_.store(instrumentPeak);
        voicePeak_.store(hybridVoice_ ? secondaryVoice_->telemetry().peak : voicePeak);
        if (tunerEnabled_.load(std::memory_order_acquire) != 0U) {
            tuner_.write(std::span<const float>(instrumentScratch_.data(), frames), asioSampleRate);
        }

        const bool recording = recorder_.recording();
        if (recording) {
            recorder_.write(jamlink::record::RecordTrack::LocalInstrument,
                std::span<const float>(instrumentScratch_.data(), frames));
            recorder_.write(jamlink::record::RecordTrack::LocalVoice,
                std::span<const float>(voiceScratch_.data(), frames));
        }
        std::fill(remoteScratch_.begin(), remoteScratch_.end(), 0.0F);
        if (peerExchange_ != nullptr) {
            peerExchange_->pushLocalAudio(
                jamlink::network::AudioStreamId::Instrument,
                std::span<const float>(instrumentScratch_.data(), frames), asioSampleRate);
            peerExchange_->pushLocalAudio(
                jamlink::network::AudioStreamId::Voice,
                std::span<const float>(voiceScratch_.data(), frames), asioSampleRate);
            for (std::size_t streamIndex = 0U;
                 streamIndex < jamlink::network::audioStreamCount; ++streamIndex) {
                static_cast<void>(peerExchange_->pullRemote48k(
                    static_cast<jamlink::network::AudioStreamId>(streamIndex),
                    std::span<float>(remoteStreamScratch_.data(), frames)));
                for (std::size_t frame = 0U; frame < frames; ++frame) {
                    remoteScratch_[frame] += remoteStreamScratch_[frame];
                }
                if (recording) {
                    recorder_.write(
                        streamIndex == 0U
                            ? jamlink::record::RecordTrack::RemoteInstrument
                            : jamlink::record::RecordTrack::RemoteVoice,
                        std::span<const float>(remoteStreamScratch_.data(), frames));
                }
            }
        } else if (recording) {
            recorder_.write(jamlink::record::RecordTrack::RemoteInstrument,
                std::span<const float>(remoteScratch_.data(), frames));
            recorder_.write(jamlink::record::RecordTrack::RemoteVoice,
                std::span<const float>(remoteScratch_.data(), frames));
        }

        const float targetInstrument = instrumentEnabled_.load(std::memory_order_acquire) != 0U
            ? instrumentGain_.load() : 0.0F;
        const float targetVoice = voiceEnabled_.load(std::memory_order_acquire) != 0U
            ? voiceGain_.load() : 0.0F;
        const float instrumentStep = (targetInstrument - currentInstrumentGain_)
            / static_cast<float>(frames);
        const float voiceStep = (targetVoice - currentVoiceGain_)
            / static_cast<float>(frames);
        const std::uint32_t toneRequest = testToneRequest_.load(std::memory_order_acquire);
        if (toneRequest != observedToneRequest_) {
            observedToneRequest_ = toneRequest;
            toneFramesRemaining_ = asioSampleRate;
            tonePhase_ = 0.0;
        }
        constexpr double twoPi = 6.28318530717958647692;
        constexpr double toneStep = twoPi * 440.0 / static_cast<double>(asioSampleRate);
        float peak = 0.0F;
        for (std::size_t frame = 0U; frame < frames; ++frame) {
            currentInstrumentGain_ += instrumentStep;
            currentVoiceGain_ += voiceStep;
            float mixed = instrumentScratch_[frame] * currentInstrumentGain_
                + voiceScratch_[frame] * currentVoiceGain_ + remoteScratch_[frame];
            if (toneFramesRemaining_ > 0U) {
                mixed += static_cast<float>(std::sin(tonePhase_) * 0.125);
                tonePhase_ += toneStep;
                if (tonePhase_ >= twoPi) {
                    tonePhase_ -= twoPi;
                }
                --toneFramesRemaining_;
            }
            mixed = std::clamp(mixed, -outputCeiling, outputCeiling);
            mixScratch_[frame] = mixed;
            peak = std::max(peak, std::abs(mixed));
        }
        currentInstrumentGain_ = targetInstrument;
        currentVoiceGain_ = targetVoice;
        outputPeak_.store(peak);
        floatToNativeSamples(
            formats_[outputLeftBuffer_],
            std::span<const float>(mixScratch_.data(), frames),
            buffers_[outputLeftBuffer_].buffers[half]);
        if (outputRightBuffer_ != outputLeftBuffer_) {
            floatToNativeSamples(
                formats_[outputRightBuffer_],
                std::span<const float>(mixScratch_.data(), frames),
                buffers_[outputRightBuffer_].buffers[half]);
        }
        static_cast<void>(ASIOOutputReady());
    }

    static void callbackBufferSwitch(long index, ASIOBool) {
        if (auto* service = active_.load(std::memory_order_acquire); service != nullptr) {
            service->process(index);
        }
    }

    static void callbackSampleRateChanged(ASIOSampleRate) {
        if (auto* service = active_.load(std::memory_order_acquire); service != nullptr) {
            service->state_.store(
                SoundcheckAudioState::DeviceInvalidated, std::memory_order_release);
        }
    }

    static long callbackMessage(long selector, long value, void*, double*) {
        if (selector == kAsioSelectorSupported) {
            return value == kAsioResetRequest || value == kAsioResyncRequest
                || value == kAsioLatenciesChanged || value == kAsioEngineVersion
                || value == kAsioSupportsTimeInfo || value == kAsioOverload ? 1L : 0L;
        }
        if (selector == kAsioEngineVersion) {
            return 2L;
        }
        if (selector == kAsioSupportsTimeInfo) {
            return 1L;
        }
        if (auto* service = active_.load(std::memory_order_acquire); service != nullptr) {
            if (selector == kAsioResetRequest || selector == kAsioResyncRequest) {
                service->overruns_.fetch_add(1U, std::memory_order_relaxed);
                service->state_.store(
                    SoundcheckAudioState::DeviceInvalidated, std::memory_order_release);
                return 1L;
            }
            if (selector == kAsioOverload) {
                service->overruns_.fetch_add(1U, std::memory_order_relaxed);
                return 1L;
            }
            if (selector == kAsioLatenciesChanged) {
                return 1L;
            }
        }
        return 0L;
    }

    static ASIOTime* callbackBufferSwitchTimeInfo(ASIOTime* time, long index, ASIOBool) {
        callbackBufferSwitch(index, ASIOTrue);
        return time;
    }

    inline static std::atomic<AsioSoundcheckAudioService*> active_{nullptr};
    ScopedAsioDriver driver_;
    std::unique_ptr<ISecondaryWasapiCapture> secondaryVoice_;
    HybridClockBridge voiceBridge_;
    std::array<ASIOBufferInfo, 4U> buffers_{};
    std::array<NativeSampleFormat, 4U> formats_{};
    ASIOCallbacks callbacks_{};
    std::size_t bufferCount_{0U};
    std::size_t instrumentBuffer_{0U};
    std::size_t voiceBuffer_{0U};
    std::size_t outputLeftBuffer_{0U};
    std::size_t outputRightBuffer_{0U};
    long bufferFrames_{0L};
    bool buffersCreated_{false};
    bool started_{false};
    bool hybridVoice_{false};
    std::vector<float> instrumentScratch_;
    std::vector<float> voiceScratch_;
    std::vector<float> remoteScratch_;
    std::vector<float> remoteStreamScratch_;
    std::vector<float> mixScratch_;
    RealtimeAtomicFloat instrumentGain_{1.0F};
    RealtimeAtomicFloat voiceGain_{1.0F};
    std::atomic<std::uint32_t> instrumentEnabled_{1U};
    std::atomic<std::uint32_t> voiceEnabled_{1U};
    std::atomic<std::uint32_t> tunerEnabled_{0U};
    std::atomic<std::uint32_t> testToneRequest_{0U};
    RealtimeAtomicFloat instrumentPeak_;
    RealtimeAtomicFloat voicePeak_;
    RealtimeAtomicFloat outputPeak_;
    std::atomic<std::uint64_t> underruns_{0U};
    std::atomic<std::uint64_t> overruns_{0U};
    std::atomic<std::int32_t> nativeError_{0};
    std::atomic<SoundcheckAudioState> state_{SoundcheckAudioState::Stopped};
    InstrumentTuner tuner_;
    jamlink::record::SessionRecorder recorder_;
    jamlink::network::IPeerAudioExchange* peerExchange_{nullptr};
    float currentInstrumentGain_{1.0F};
    float currentVoiceGain_{1.0F};
    std::uint32_t observedToneRequest_{0U};
    std::uint32_t toneFramesRemaining_{0U};
    double tonePhase_{0.0};
};

} // namespace

std::unique_ptr<ISoundcheckAudioService> createAsioSoundcheckAudioService() {
    return std::make_unique<AsioSoundcheckAudioService>();
}

} // namespace jamlink::audio
