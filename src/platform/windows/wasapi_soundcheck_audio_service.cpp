// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#define NOMINMAX
#include <Windows.h>

#include "jamlink/audio/async_mono_resampler.hpp"
#include "jamlink/audio/realtime_atomic.hpp"
#include "jamlink/audio/soundcheck_audio_service.hpp"
#include "jamlink/network/peer_audio_transport.hpp"
#include "windows_audio_services.hpp"

#include <audioclient.h>
#include <avrt.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace jamlink::audio {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t resamplerCapacityFrames = 65'536U;
constexpr float outputCeiling = 0.98F;
// The transport always delivers 48 kHz packets of this size.
constexpr std::uint32_t networkSampleRate = 48'000U;
constexpr std::size_t networkPacketFrames = 240U;

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != nullptr; }
    void reset(HANDLE replacement = nullptr) noexcept {
        if (value_ != nullptr) {
            static_cast<void>(CloseHandle(value_));
        }
        value_ = replacement;
    }

private:
    HANDLE value_{nullptr};
};

class ComApartment final {
public:
    explicit ComApartment(DWORD model = COINIT_MULTITHREADED) noexcept {
        const HRESULT result = CoInitializeEx(nullptr, model);
        initialized_ = result == S_OK || result == S_FALSE;
        available_ = initialized_ || result == RPC_E_CHANGED_MODE;
    }
    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool available() const noexcept { return available_; }

private:
    bool initialized_{false};
    bool available_{false};
};

struct CoTaskMemWaveFormatDeleter final {
    void operator()(WAVEFORMATEX* format) const noexcept { CoTaskMemFree(format); }
};
using UniqueWaveFormat = std::unique_ptr<WAVEFORMATEX, CoTaskMemWaveFormatDeleter>;

enum class SampleEncoding : std::uint8_t {
    Float32,
    Pcm16,
    Pcm24,
    Pcm32,
    Unsupported
};

struct NativeFormat final {
    SampleEncoding encoding{SampleEncoding::Unsupported};
    std::uint32_t sampleRate{0U};
    std::uint16_t channelCount{0U};
    std::uint16_t blockAlign{0U};
    std::uint16_t bytesPerSample{0U};
};

[[nodiscard]] NativeFormat describeFormat(const WAVEFORMATEX& format) noexcept {
    NativeFormat result;
    result.sampleRate = format.nSamplesPerSec;
    result.channelCount = format.nChannels;
    result.blockAlign = format.nBlockAlign;
    result.bytesPerSample = static_cast<std::uint16_t>(format.wBitsPerSample / 8U);

    WORD tag = format.wFormatTag;
    GUID subFormat{};
    if (tag == WAVE_FORMAT_EXTENSIBLE
        && format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        subFormat = extensible.SubFormat;
        if (subFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (subFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            tag = WAVE_FORMAT_PCM;
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32U) {
        result.encoding = SampleEncoding::Float32;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16U) {
        result.encoding = SampleEncoding::Pcm16;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 24U) {
        result.encoding = SampleEncoding::Pcm24;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32U) {
        result.encoding = SampleEncoding::Pcm32;
    }
    return result;
}

[[nodiscard]] float decodeSample(
    const BYTE* data,
    const NativeFormat& format,
    std::uint32_t frame,
    std::uint32_t channel) noexcept {
    const auto* sample = data + static_cast<std::size_t>(frame) * format.blockAlign
        + static_cast<std::size_t>(channel) * format.bytesPerSample;
    switch (format.encoding) {
    case SampleEncoding::Float32: {
        float value = 0.0F;
        std::memcpy(&value, sample, sizeof(value));
        return std::isfinite(value) ? value : 0.0F;
    }
    case SampleEncoding::Pcm16: {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<float>(value) / 32'768.0F;
    }
    case SampleEncoding::Pcm24: {
        std::int32_t value = static_cast<std::int32_t>(sample[0])
            | (static_cast<std::int32_t>(sample[1]) << 8)
            | (static_cast<std::int32_t>(sample[2]) << 16);
        if ((value & 0x0080'0000) != 0) {
            value |= static_cast<std::int32_t>(0xFF00'0000U);
        }
        return static_cast<float>(value) / 8'388'608.0F;
    }
    case SampleEncoding::Pcm32: {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<float>(static_cast<double>(value) / 2'147'483'648.0);
    }
    case SampleEncoding::Unsupported:
        return 0.0F;
    }
    return 0.0F;
}

void encodeSample(
    BYTE* data,
    const NativeFormat& format,
    std::uint32_t frame,
    std::uint32_t channel,
    float input) noexcept {
    const float value = std::clamp(std::isfinite(input) ? input : 0.0F, -outputCeiling, outputCeiling);
    auto* sample = data + static_cast<std::size_t>(frame) * format.blockAlign
        + static_cast<std::size_t>(channel) * format.bytesPerSample;
    switch (format.encoding) {
    case SampleEncoding::Float32:
        std::memcpy(sample, &value, sizeof(value));
        break;
    case SampleEncoding::Pcm16: {
        const auto converted = static_cast<std::int16_t>(std::lrint(value * 32'767.0F));
        std::memcpy(sample, &converted, sizeof(converted));
        break;
    }
    case SampleEncoding::Pcm24: {
        const auto converted = static_cast<std::int32_t>(std::lrint(value * 8'388'607.0F));
        sample[0] = static_cast<BYTE>(converted & 0xFF);
        sample[1] = static_cast<BYTE>((converted >> 8) & 0xFF);
        sample[2] = static_cast<BYTE>((converted >> 16) & 0xFF);
        break;
    }
    case SampleEncoding::Pcm32: {
        const auto converted = static_cast<std::int32_t>(
            std::llrint(static_cast<double>(value) * 2'147'483'647.0));
        std::memcpy(sample, &converted, sizeof(converted));
        break;
    }
    case SampleEncoding::Unsupported:
        break;
    }
}

[[nodiscard]] std::wstring widen(const std::string& input) {
    if (input.empty()) {
        return {};
    }
    const int count = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (count <= 0) {
        return {};
    }
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    static_cast<void>(MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()),
        output.data(), count));
    return output;
}

[[nodiscard]] std::string narrow(const wchar_t* input) {
    if (input == nullptr || *input == L'\0') {
        return {};
    }
    const int inputLength = static_cast<int>(std::wcslen(input));
    const int count = WideCharToMultiByte(
        CP_UTF8, 0, input, inputLength, nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(count), '\0');
    static_cast<void>(WideCharToMultiByte(
        CP_UTF8, 0, input, inputLength, output.data(), count, nullptr, nullptr));
    return output;
}

[[nodiscard]] std::string friendlyName(IMMDevice& device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device.OpenPropertyStore(STGM_READ, &properties))) {
        return "Windows audio device";
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    const HRESULT result = properties->GetValue(PKEY_Device_FriendlyName, &value);
    std::string name = "Windows audio device";
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) {
        name = narrow(value.pwszVal);
    }
    static_cast<void>(PropVariantClear(&value));
    return name;
}

[[nodiscard]] std::vector<std::uint32_t> periodOptions(
    IAudioClient& client,
    const WAVEFORMATEX& format) {
    std::vector<std::uint32_t> result;
    ComPtr<IAudioClient3> client3;
    if (SUCCEEDED(client.QueryInterface(IID_PPV_ARGS(&client3)))) {
        UINT32 defaultFrames = 0U;
        UINT32 fundamentalFrames = 0U;
        UINT32 minimumFrames = 0U;
        UINT32 maximumFrames = 0U;
        if (SUCCEEDED(client3->GetSharedModeEnginePeriod(
                &format, &defaultFrames, &fundamentalFrames,
                &minimumFrames, &maximumFrames))) {
            result = {minimumFrames, defaultFrames, maximumFrames};
            std::sort(result.begin(), result.end());
            result.erase(std::remove(result.begin(), result.end(), 0U), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
        }
    }
    if (result.empty()) {
        REFERENCE_TIME defaultPeriod = 0;
        REFERENCE_TIME minimumPeriod = 0;
        if (SUCCEEDED(client.GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
            const auto toFrames = [&format](REFERENCE_TIME period) {
                return static_cast<std::uint32_t>(std::max<REFERENCE_TIME>(
                    1, (period * format.nSamplesPerSec + 9'999'999) / 10'000'000));
            };
            result = {toFrames(minimumPeriod), toFrames(defaultPeriod)};
            std::sort(result.begin(), result.end());
            result.erase(std::unique(result.begin(), result.end()), result.end());
        }
    }
    return result;
}

[[nodiscard]] SoundcheckDeviceInventory enumerateWasapiEndpoints() {
    SoundcheckDeviceInventory inventory;
    ComApartment apartment(COINIT_APARTMENTTHREADED);
    if (!apartment.available()) {
        return inventory;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator)))) {
        return inventory;
    }

    const auto defaultEndpointId = [&enumerator](EDataFlow flow) {
        std::string result;
        ComPtr<IMMDevice> endpoint;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(flow, eMultimedia, &endpoint))) {
            LPWSTR allocatedId = nullptr;
            if (SUCCEEDED(endpoint->GetId(&allocatedId)) && allocatedId != nullptr) {
                result = narrow(allocatedId);
            }
            CoTaskMemFree(allocatedId);
        }
        return result;
    };
    const std::string defaultCaptureId = defaultEndpointId(eCapture);
    const std::string defaultRenderId = defaultEndpointId(eRender);

    const auto enumerateDirection = [&enumerator, &inventory](
                                        EDataFlow flow,
                                        bool capture) {
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(
                flow, DEVICE_STATE_ACTIVE, &collection))) {
            return;
        }
        UINT count = 0U;
        if (FAILED(collection->GetCount(&count))) {
            return;
        }
        for (UINT index = 0U; index < count; ++index) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(index, &device))) {
                continue;
            }
            LPWSTR allocatedId = nullptr;
            if (FAILED(device->GetId(&allocatedId)) || allocatedId == nullptr) {
                CoTaskMemFree(allocatedId);
                continue;
            }
            const std::string endpointId = narrow(allocatedId);
            CoTaskMemFree(allocatedId);

            ComPtr<IAudioClient> client;
            if (FAILED(device->Activate(
                    __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                    reinterpret_cast<void**>(client.GetAddressOf())))) {
                continue;
            }
            WAVEFORMATEX* allocatedFormat = nullptr;
            if (FAILED(client->GetMixFormat(&allocatedFormat)) || allocatedFormat == nullptr) {
                CoTaskMemFree(allocatedFormat);
                continue;
            }
            UniqueWaveFormat format(allocatedFormat);
            const NativeFormat native = describeFormat(*format);
            if (native.encoding == SampleEncoding::Unsupported
                || native.channelCount == 0U || native.sampleRate == 0U) {
                continue;
            }
            const std::string name = friendlyName(*device.Get());
            const auto periods = periodOptions(*client.Get(), *format);
            if (capture) {
                for (std::uint32_t channel = 0U; channel < native.channelCount; ++channel) {
                    inventory.inputOptions.push_back(SoundcheckEndpointOption{
                        endpointId,
                        name + " — Input " + std::to_string(channel + 1U),
                        channel,
                        0U,
                        false,
                        native.sampleRate,
                        periods});
                }
            } else {
                for (std::uint32_t channel = 0U; channel < native.channelCount; channel += 2U) {
                    const bool stereo = channel + 1U < native.channelCount;
                    inventory.outputOptions.push_back(SoundcheckEndpointOption{
                        endpointId,
                        name + (stereo
                            ? " — Output " + std::to_string(channel + 1U) + "–"
                                + std::to_string(channel + 2U)
                            : " — Output " + std::to_string(channel + 1U)),
                        channel,
                        stereo ? channel + 1U : channel,
                        stereo,
                        native.sampleRate,
                        periods});
                }
            }
        }
    };

    enumerateDirection(eCapture, true);
    enumerateDirection(eRender, false);
    const auto defaultFirst = [](auto& options, const std::string& defaultId) {
        std::stable_partition(options.begin(), options.end(), [&defaultId](const auto& option) {
            return option.endpointId == defaultId;
        });
    };
    defaultFirst(inventory.inputOptions, defaultCaptureId);
    defaultFirst(inventory.outputOptions, defaultRenderId);
    return inventory;
}

struct WasapiStream final {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    ComPtr<IAudioRenderClient> render;
    UniqueWaveFormat waveFormat;
    NativeFormat format;
    UniqueHandle event;
    UINT32 bufferFrames{0U};
    std::uint32_t selectedPrimary{0U};
    std::uint32_t selectedSecondary{0U};
    bool hasSecondary{false};
};

[[nodiscard]] bool configureClientProperties(IAudioClient& client) noexcept {
    ComPtr<IAudioClient2> client2;
    if (FAILED(client.QueryInterface(IID_PPV_ARGS(&client2)))) {
        return false;
    }
    AudioClientProperties properties{};
    properties.cbSize = sizeof(properties);
    properties.bIsOffload = FALSE;
    properties.eCategory = AudioCategory_Media;
    properties.Options = AUDCLNT_STREAMOPTIONS_NONE;
    return SUCCEEDED(client2->SetClientProperties(&properties));
}

[[nodiscard]] HRESULT initializeStream(
    IMMDeviceEnumerator& enumerator,
    const SoundcheckEndpointOption& option,
    bool capture,
    std::uint32_t requestedFrames,
    WasapiStream& stream) {
    const std::wstring endpointId = widen(option.endpointId);
    if (endpointId.empty()) {
        return E_INVALIDARG;
    }
    ComPtr<IMMDevice> device;
    HRESULT result = enumerator.GetDevice(endpointId.c_str(), &device);
    if (FAILED(result)) {
        return result;
    }
    result = device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(stream.client.GetAddressOf()));
    if (FAILED(result)) {
        return result;
    }
    static_cast<void>(configureClientProperties(*stream.client.Get()));

    WAVEFORMATEX* allocatedFormat = nullptr;
    result = stream.client->GetMixFormat(&allocatedFormat);
    if (FAILED(result) || allocatedFormat == nullptr) {
        CoTaskMemFree(allocatedFormat);
        return FAILED(result) ? result : E_FAIL;
    }
    stream.waveFormat.reset(allocatedFormat);
    stream.format = describeFormat(*stream.waveFormat);
    if (stream.format.encoding == SampleEncoding::Unsupported
        || option.primaryChannel >= stream.format.channelCount
        || (option.hasSecondaryChannel && option.secondaryChannel >= stream.format.channelCount)) {
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }

    // InitializeSharedAudioStream accepts only a limited flag subset. In
    // particular, AUDCLNT_STREAMFLAGS_NOPERSIST is valid for Initialize but
    // is rejected by IAudioClient3 with AUDCLNT_E_INVALID_STREAM_FLAG.
    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    ComPtr<IAudioClient3> client3;
    if (SUCCEEDED(stream.client.As(&client3))) {
        UINT32 defaultFrames = 0U;
        UINT32 fundamentalFrames = 0U;
        UINT32 minimumFrames = 0U;
        UINT32 maximumFrames = 0U;
        result = client3->GetSharedModeEnginePeriod(
            stream.waveFormat.get(), &defaultFrames, &fundamentalFrames,
            &minimumFrames, &maximumFrames);
        if (SUCCEEDED(result)) {
            UINT32 periodFrames = minimumFrames;
            if (requestedFrames == defaultFrames || requestedFrames == maximumFrames
                || requestedFrames == minimumFrames) {
                periodFrames = requestedFrames;
            }
            result = client3->InitializeSharedAudioStream(
                flags, periodFrames, stream.waveFormat.get(), nullptr);
        }
    } else {
        result = stream.client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, stream.waveFormat.get(), nullptr);
    }
    if (FAILED(result)) {
        return result;
    }

    stream.event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!stream.event) {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    result = stream.client->SetEventHandle(stream.event.get());
    if (FAILED(result)) {
        return result;
    }
    result = stream.client->GetBufferSize(&stream.bufferFrames);
    if (FAILED(result)) {
        return result;
    }
    if (capture) {
        result = stream.client->GetService(IID_PPV_ARGS(&stream.capture));
    } else {
        result = stream.client->GetService(IID_PPV_ARGS(&stream.render));
    }
    if (FAILED(result)) {
        return result;
    }
    stream.selectedPrimary = option.primaryChannel;
    stream.selectedSecondary = option.secondaryChannel;
    stream.hasSecondary = option.hasSecondaryChannel;
    return S_OK;
}

[[nodiscard]] SoundcheckAudioState stateForFailure(HRESULT result) noexcept {
    if (result == AUDCLNT_E_DEVICE_INVALIDATED || result == AUDCLNT_E_RESOURCES_INVALIDATED) {
        return SoundcheckAudioState::DeviceInvalidated;
    }
    if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        return SoundcheckAudioState::UnsupportedFormat;
    }
    if (result == E_NOTFOUND || result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return SoundcheckAudioState::DeviceUnavailable;
    }
    return SoundcheckAudioState::InitializationFailed;
}

class WasapiSoundcheckAudioService final : public ISoundcheckAudioService {
public:
    WasapiSoundcheckAudioService() = default;
    ~WasapiSoundcheckAudioService() override { stop(); }
    WasapiSoundcheckAudioService(const WasapiSoundcheckAudioService&) = delete;
    WasapiSoundcheckAudioService& operator=(const WasapiSoundcheckAudioService&) = delete;

    [[nodiscard]] SoundcheckDeviceInventory enumerate() override {
        return enumerateWasapiEndpoints();
    }

    [[nodiscard]] bool start(const SoundcheckAudioConfiguration& configuration) override {
        stop();
        clearAllSignalHealth();
        if (configuration.instrument.endpointId.empty()
            || configuration.voice.endpointId.empty()
            || configuration.output.endpointId.empty()) {
            state_.store(SoundcheckAudioState::NoEndpoints, std::memory_order_release);
            return false;
        }

        stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent_) {
            state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
            return false;
        }
        setMonitorControls(
            configuration.instrumentGain,
            configuration.instrumentEnabled,
            configuration.voiceGain,
            configuration.voiceEnabled);
        state_.store(SoundcheckAudioState::Starting, std::memory_order_release);
        underruns_.store(0U, std::memory_order_relaxed);
        overruns_.store(0U, std::memory_order_relaxed);
        instrumentPeak_.store(0.0F);
        voicePeak_.store(0.0F);
        outputPeak_.store(0.0F);
        outputSampleRate_.store(0U, std::memory_order_relaxed);
        outputBufferFrames_.store(0U, std::memory_order_relaxed);
        nativeError_.store(0, std::memory_order_relaxed);
        testToneRequest_.store(0U, std::memory_order_relaxed);

        std::promise<bool> initialized;
        auto result = initialized.get_future();
        worker_ = std::thread(
            [this, configuration, ready = std::move(initialized)]() mutable {
                run(configuration, std::move(ready));
            });
        const bool started = result.get();
        if (!started && worker_.joinable()) {
            worker_.join();
            stopEvent_.reset();
        }
        return started;
    }

    void stop() noexcept override {
        if (stopEvent_) {
            static_cast<void>(SetEvent(stopEvent_.get()));
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        stopEvent_.reset();
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
        if (tunerEnabled_.load(std::memory_order_acquire) == 0U) {
            return {};
        }
        return tuner_.analyse();
    }

    [[nodiscard]] bool startRecording(
        const std::filesystem::path& directory,
        const std::string& sessionName) override {
        const std::uint32_t rate = outputSampleRate_.load(std::memory_order_relaxed);
        if (rate == 0U) {
            return false;
        }
        recordingInstrumentHealth_.clearClipLatch();
        recordingVoiceHealth_.clearClipLatch();
        // The originals keep their capture device's rate. Resampling them to
        // the timeline would make them no longer originals.
        return recorder_.start(
            directory, sessionName, rate,
            instrumentCaptureRate_.load(std::memory_order_relaxed),
            voiceCaptureRate_.load(std::memory_order_relaxed),
            recordSelection_);
    }

    void setRecordTrackSelection(
        const jamlink::record::RecordTrackSelection& selection) noexcept override {
        recordSelection_ = selection;
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

    void clearSignalHealth(SignalHealthPath path) noexcept override {
        switch (path) {
        case SignalHealthPath::InstrumentInput:
            instrumentInputHealth_.clearClipLatch();
            instrumentSourceClipped_.store(0U, std::memory_order_release);
            break;
        case SignalHealthPath::VoiceInput:
            voiceInputHealth_.clearClipLatch();
            voiceSourceClipped_.store(0U, std::memory_order_release);
            break;
        case SignalHealthPath::InstrumentSend:
            instrumentSendHealth_.clearClipLatch();
            break;
        case SignalHealthPath::VoiceSend:
            voiceSendHealth_.clearClipLatch();
            break;
        case SignalHealthPath::MonitorMix:
            monitorMixHealth_.clearClipLatch();
            break;
        case SignalHealthPath::RecordingInstrument:
            recordingInstrumentHealth_.clearClipLatch();
            break;
        case SignalHealthPath::RecordingVoice:
            recordingVoiceHealth_.clearClipLatch();
            break;
        }
    }

    void requestSignalHealthSelfTest(SignalHealthPath path) noexcept override {
        switch (path) {
        case SignalHealthPath::InstrumentInput:
            instrumentInputHealth_.requestClipSelfTest();
            break;
        case SignalHealthPath::VoiceInput:
            voiceInputHealth_.requestClipSelfTest();
            break;
        case SignalHealthPath::InstrumentSend:
            instrumentSendHealth_.requestClipSelfTest();
            break;
        case SignalHealthPath::VoiceSend:
            voiceSendHealth_.requestClipSelfTest();
            break;
        case SignalHealthPath::MonitorMix:
            monitorMixHealth_.requestClipSelfTest();
            break;
        case SignalHealthPath::RecordingInstrument:
            recordingInstrumentHealth_.requestClipSelfTest();
            break;
        case SignalHealthPath::RecordingVoice:
            recordingVoiceHealth_.requestClipSelfTest();
            break;
        }
    }

    [[nodiscard]] SoundcheckAudioTelemetry telemetry() const noexcept override {
        return SoundcheckAudioTelemetry{
            state_.load(std::memory_order_acquire),
            instrumentPeak_.load(),
            voicePeak_.load(),
            outputPeak_.load(),
            outputSampleRate_.load(std::memory_order_relaxed),
            outputBufferFrames_.load(std::memory_order_relaxed),
            underruns_.load(std::memory_order_relaxed),
            overruns_.load(std::memory_order_relaxed),
            false,
            nativeError_.load(std::memory_order_relaxed),
            true,
            0,
            signalHealthTelemetry(instrumentInputHealth_.snapshot()),
            signalHealthTelemetry(voiceInputHealth_.snapshot()),
            signalHealthTelemetry(instrumentSendHealth_.snapshot()),
            signalHealthTelemetry(voiceSendHealth_.snapshot()),
            signalHealthTelemetry(monitorMixHealth_.snapshot()),
            signalHealthTelemetry(recordingInstrumentHealth_.snapshot()),
            signalHealthTelemetry(recordingVoiceHealth_.snapshot())};
    }

private:
    void clearAllSignalHealth() noexcept {
        instrumentInputHealth_.clearClipLatch();
        voiceInputHealth_.clearClipLatch();
        instrumentSendHealth_.clearClipLatch();
        voiceSendHealth_.clearClipLatch();
        monitorMixHealth_.clearClipLatch();
        recordingInstrumentHealth_.clearClipLatch();
        recordingVoiceHealth_.clearClipLatch();
        instrumentSourceClipped_.store(0U, std::memory_order_relaxed);
        voiceSourceClipped_.store(0U, std::memory_order_relaxed);
    }

    struct InitializationResult final {
        bool succeeded{false};
        SoundcheckAudioState failure{SoundcheckAudioState::InitializationFailed};
        HRESULT nativeError{S_OK};
    };

    [[nodiscard]] InitializationResult initialize(
        const SoundcheckAudioConfiguration& configuration,
        WasapiStream& instrument,
        WasapiStream& voice,
        WasapiStream& output) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = CoCreateInstance(
            __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) {
            return {false, stateForFailure(result), result};
        }
        result = initializeStream(
            *enumerator.Get(), configuration.output, false,
            configuration.requestedBufferFrames, output);
        if (FAILED(result)) {
            return {false, stateForFailure(result), result};
        }
        result = initializeStream(
            *enumerator.Get(), configuration.instrument, true, 0U, instrument);
        if (FAILED(result)) {
            return {false, stateForFailure(result), result};
        }
        result = initializeStream(
            *enumerator.Get(), configuration.voice, true, 0U, voice);
        if (FAILED(result)) {
            return {false, stateForFailure(result), result};
        }
        return {true, SoundcheckAudioState::Running, S_OK};
    }

    void run(
        const SoundcheckAudioConfiguration& configuration,
        std::promise<bool> initialized) noexcept {
        bool promiseCompleted = false;
        try {
            ComApartment apartment;
            if (!apartment.available()) {
                state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
                initialized.set_value(false);
                return;
            }
            DWORD taskIndex = 0U;
            HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

            WasapiStream instrument;
            WasapiStream voice;
            WasapiStream output;
            const InitializationResult setup = initialize(
                configuration, instrument, voice, output);
            if (!setup.succeeded) {
                nativeError_.store(
                    static_cast<std::int32_t>(setup.nativeError),
                    std::memory_order_relaxed);
                state_.store(setup.failure, std::memory_order_release);
                initialized.set_value(false);
                if (mmcss != nullptr) {
                    static_cast<void>(AvRevertMmThreadCharacteristics(mmcss));
                }
                return;
            }

            AsyncMonoResampler instrumentBuffer(resamplerCapacityFrames);
            AsyncMonoResampler voiceBuffer(resamplerCapacityFrames);
            std::array<AsyncMonoResampler, jamlink::network::audioStreamCount> remoteBuffers{
                AsyncMonoResampler(resamplerCapacityFrames),
                AsyncMonoResampler(resamplerCapacityFrames)};
            instrumentBuffer.configure(instrument.format.sampleRate, output.format.sampleRate);
            voiceBuffer.configure(voice.format.sampleRate, output.format.sampleRate);
            for (auto& buffer : remoteBuffers) {
                buffer.configure(networkSampleRate, output.format.sampleRate);
            }
            std::vector<float> instrumentCapture(instrument.bufferFrames, 0.0F);
            std::vector<float> voiceCapture(voice.bufferFrames, 0.0F);
            std::vector<float> instrumentRender(output.bufferFrames, 0.0F);
            std::vector<float> voiceRender(output.bufferFrames, 0.0F);
            std::vector<float> remoteRender(output.bufferFrames, 0.0F);
            std::vector<float> remoteStreamRender(output.bufferFrames, 0.0F);
            std::vector<float> remoteNetworkFrames(
                std::max<UINT32>(output.bufferFrames * 2U, 1'024U), 0.0F);

            outputSampleRate_.store(output.format.sampleRate, std::memory_order_relaxed);
            instrumentCaptureRate_.store(
                instrument.format.sampleRate, std::memory_order_relaxed);
            voiceCaptureRate_.store(voice.format.sampleRate, std::memory_order_relaxed);
            outputBufferFrames_.store(output.bufferFrames, std::memory_order_relaxed);

            HRESULT result = instrument.client->Start();
            if (SUCCEEDED(result)) {
                result = voice.client->Start();
            }
            if (SUCCEEDED(result)) {
                result = output.client->Start();
            }
            if (FAILED(result)) {
                nativeError_.store(static_cast<std::int32_t>(result), std::memory_order_relaxed);
                state_.store(stateForFailure(result), std::memory_order_release);
                initialized.set_value(false);
                if (mmcss != nullptr) {
                    static_cast<void>(AvRevertMmThreadCharacteristics(mmcss));
                }
                return;
            }

            state_.store(SoundcheckAudioState::Running, std::memory_order_release);
            initialized.set_value(true);
            promiseCompleted = true;

            const HANDLE waits[] = {
                stopEvent_.get(),
                instrument.event.get(),
                voice.event.get(),
                output.event.get()};
            float currentInstrumentGain = 0.0F;
            float currentVoiceGain = 0.0F;
            std::uint32_t observedToneRequest = 0U;
            std::uint32_t toneFramesRemaining = 0U;
            double tonePhase = 0.0;
            bool keepRunning = true;
            while (keepRunning) {
                const DWORD waitResult = WaitForMultipleObjects(
                    static_cast<DWORD>(std::size(waits)), waits, FALSE, INFINITE);
                switch (waitResult) {
                case WAIT_OBJECT_0:
                    keepRunning = false;
                    break;
                case WAIT_OBJECT_0 + 1U:
                    keepRunning = processCapture(
                        instrument, instrumentCapture, instrumentBuffer, instrumentPeak_,
                        instrumentInputHealth_, instrumentSendHealth_,
                        instrumentSourceClipped_,
                        jamlink::network::AudioStreamId::Instrument, true);
                    break;
                case WAIT_OBJECT_0 + 2U:
                    keepRunning = processCapture(
                        voice, voiceCapture, voiceBuffer, voicePeak_, voiceInputHealth_,
                        voiceSendHealth_, voiceSourceClipped_,
                        jamlink::network::AudioStreamId::Voice, false);
                    break;
                case WAIT_OBJECT_0 + 3U:
                    keepRunning = processRender(
                        output,
                        instrumentBuffer,
                        voiceBuffer,
                        instrumentRender,
                        voiceRender,
                        remoteRender,
                        remoteStreamRender,
                        remoteNetworkFrames,
                        remoteBuffers,
                        currentInstrumentGain,
                        currentVoiceGain,
                        observedToneRequest,
                        toneFramesRemaining,
                        tonePhase);
                    break;
                default:
                    keepRunning = false;
                    state_.store(SoundcheckAudioState::DeviceInvalidated, std::memory_order_release);
                    break;
                }
            }

            static_cast<void>(output.client->Stop());
            static_cast<void>(voice.client->Stop());
            static_cast<void>(instrument.client->Stop());
            if (state_.load(std::memory_order_acquire) == SoundcheckAudioState::Running) {
                state_.store(SoundcheckAudioState::Stopped, std::memory_order_release);
            }
            if (mmcss != nullptr) {
                static_cast<void>(AvRevertMmThreadCharacteristics(mmcss));
            }
        } catch (...) {
            state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
            if (!promiseCompleted) {
                initialized.set_value(false);
            }
        }
    }

    [[nodiscard]] bool processCapture(
        WasapiStream& stream,
        std::vector<float>& scratch,
        AsyncMonoResampler& destination,
        RealtimeAtomicFloat& peakTarget,
        LevelMeter& inputHealth,
        LevelMeter& sendHealth,
        std::atomic<std::uint32_t>& sourceClipped,
        jamlink::network::AudioStreamId streamId,
        bool feedTuner) noexcept {
        UINT32 packetFrames = 0U;
        HRESULT result = stream.capture->GetNextPacketSize(&packetFrames);
        while (SUCCEEDED(result) && packetFrames != 0U) {
            BYTE* data = nullptr;
            UINT32 frames = 0U;
            DWORD flags = 0U;
            result = stream.capture->GetBuffer(
                &data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) {
                state_.store(stateForFailure(result), std::memory_order_release);
                return false;
            }
            if (frames > scratch.size()) {
                static_cast<void>(stream.capture->ReleaseBuffer(frames));
                state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
                return false;
            }
            float peak = 0.0F;
            for (UINT32 frame = 0U; frame < frames; ++frame) {
                const float sample = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U
                    ? 0.0F
                    : decodeSample(data, stream.format, frame, stream.selectedPrimary);
                scratch[frame] = sample;
                peak = std::max(peak, std::abs(sample));
            }
            peakTarget.store(std::clamp(peak, 0.0F, 1.0F));
            if (peak >= LevelMeter::nativeInputClipThreshold) {
                sourceClipped.store(1U, std::memory_order_release);
            }
            inputHealth.process(std::span<const float>(scratch.data(), frames));
            if (feedTuner && tunerEnabled_.load(std::memory_order_acquire) != 0U) {
                // A copy taken at the input, before monitoring or transmission,
                // so tuning never changes what is heard or sent.
                tuner_.write(
                    std::span<const float>(scratch.data(), frames),
                    stream.format.sampleRate);
            }
            static_cast<void>(destination.write(
                std::span<const float>(scratch.data(), frames)));
            // The local original: what was played, captured before the monitor
            // converter and before anything the network could do to it. The
            // aligned live tracks in processRender are what was heard; a
            // dropout damages those and cannot touch this.
            if (recorder_.recording()) {
                recorder_.write(
                    streamId == jamlink::network::AudioStreamId::Instrument
                        ? jamlink::record::RecordTrack::LocalInstrumentOriginal
                        : jamlink::record::RecordTrack::LocalVoiceOriginal,
                    std::span<const float>(scratch.data(), frames));
            }
            // What the other person hears is taken here, from captured audio at
            // the capture device's own rate, and not from the render callback.
            //
            // Feeding the network from the render side made the outgoing stream
            // a function of the playback device. On shared-mode Windows audio
            // the capture and render endpoints run on independent clocks with a
            // converter between them, and that converter zero-fills whatever it
            // cannot supply. Its return value was discarded, so every underrun
            // put a block of digital silence on the wire as though it were the
            // guitar, and the send rate sat well under the one packet per five
            // milliseconds the stream owed. Field testing heard the result as
            // bit-crushing and glitching on a link with a 4 ms round trip.
            if (peerExchange_ != nullptr) {
                sendHealth.process(std::span<const float>(scratch.data(), frames));
                peerExchange_->pushLocalAudio(
                    streamId,
                    std::span<const float>(scratch.data(), frames),
                    stream.format.sampleRate);
            }
            result = stream.capture->ReleaseBuffer(frames);
            if (FAILED(result)) {
                state_.store(stateForFailure(result), std::memory_order_release);
                return false;
            }
            result = stream.capture->GetNextPacketSize(&packetFrames);
        }
        if (FAILED(result)) {
            state_.store(stateForFailure(result), std::memory_order_release);
            return false;
        }
        return true;
    }

    [[nodiscard]] bool processRender(
        WasapiStream& output,
        AsyncMonoResampler& instrumentBuffer,
        AsyncMonoResampler& voiceBuffer,
        std::vector<float>& instrumentScratch,
        std::vector<float>& voiceScratch,
        std::vector<float>& remoteScratch,
        std::vector<float>& remoteStreamScratch,
        std::vector<float>& remoteNetworkScratch,
        std::array<AsyncMonoResampler, jamlink::network::audioStreamCount>& remoteBuffers,
        float& currentInstrumentGain,
        float& currentVoiceGain,
        std::uint32_t& observedToneRequest,
        std::uint32_t& toneFramesRemaining,
        double& tonePhase) noexcept {
        UINT32 padding = 0U;
        HRESULT result = output.client->GetCurrentPadding(&padding);
        if (FAILED(result)) {
            state_.store(stateForFailure(result), std::memory_order_release);
            return false;
        }
        const UINT32 frames = output.bufferFrames - padding;
        if (frames == 0U) {
            return true;
        }
        if (frames > instrumentScratch.size() || frames > voiceScratch.size()) {
            state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
            return false;
        }
        static_cast<void>(instrumentBuffer.read(
            std::span<float>(instrumentScratch.data(), frames)));
        static_cast<void>(voiceBuffer.read(
            std::span<float>(voiceScratch.data(), frames)));
        std::fill(remoteScratch.begin(), remoteScratch.begin() + frames, 0.0F);

        // Recording taps the sources, not the mix, so the take stays separable.
        // Every track advances by the same frame count on every callback, with
        // silence where a source is absent, which keeps the files aligned to a
        // single timeline.
        const bool capturing = recorder_.recording();
        if (capturing) {
            recordingInstrumentHealth_.process(
                std::span<const float>(instrumentScratch.data(), frames));
            recordingVoiceHealth_.process(
                std::span<const float>(voiceScratch.data(), frames));
            recorder_.write(
                jamlink::record::RecordTrack::LocalInstrument,
                std::span<const float>(instrumentScratch.data(), frames));
            recorder_.write(
                jamlink::record::RecordTrack::LocalVoice,
                std::span<const float>(voiceScratch.data(), frames));
        }

        if (peerExchange_ != nullptr) {
            peerExchange_->setLocalStreamClipState(
                jamlink::network::AudioStreamId::Instrument,
                instrumentSourceClipped_.load(std::memory_order_acquire) != 0U);
            peerExchange_->setLocalStreamClipState(
                jamlink::network::AudioStreamId::Voice,
                voiceSourceClipped_.load(std::memory_order_acquire) != 0U);
            // Instrument and voice travel as independent streams, so each has
            // its own receive buffer and neither is summed before transmission.
            // They are handed to the transport from the capture callbacks, not
            // from here, so that the outgoing audio is the audio that was
            // played rather than whatever the playback device had room for.
            for (std::size_t index = 0U;
                 index < jamlink::network::audioStreamCount; ++index) {
                const auto stream = static_cast<jamlink::network::AudioStreamId>(index);
                auto& streamBuffer = remoteBuffers[index];
                if (output.format.sampleRate == networkSampleRate) {
                    // Rates already match, so skip the converter entirely and
                    // avoid paying its buffering latency in the common case.
                    static_cast<void>(peerExchange_->pullRemote48k(
                        stream, std::span<float>(remoteStreamScratch.data(), frames)));
                } else {
                    const std::size_t perCallback = static_cast<std::size_t>(
                        (static_cast<std::uint64_t>(frames) * networkSampleRate
                         + output.format.sampleRate - 1U)
                        / output.format.sampleRate);
                    const std::size_t target = perCallback * 2U + networkPacketFrames;
                    while (streamBuffer.availableFrames() < target
                           && networkPacketFrames <= remoteNetworkScratch.size()) {
                        static_cast<void>(peerExchange_->pullRemote48k(
                            stream,
                            std::span<float>(
                                remoteNetworkScratch.data(), networkPacketFrames)));
                        static_cast<void>(streamBuffer.write(
                            std::span<const float>(
                                remoteNetworkScratch.data(), networkPacketFrames)));
                    }
                    static_cast<void>(streamBuffer.read(
                        std::span<float>(remoteStreamScratch.data(), frames)));
                }
                for (UINT32 frame = 0U; frame < frames; ++frame) {
                    remoteScratch[frame] += remoteStreamScratch[frame];
                }
                if (capturing) {
                    recorder_.write(
                        index == 0U
                            ? jamlink::record::RecordTrack::RemoteInstrument
                            : jamlink::record::RecordTrack::RemoteVoice,
                        std::span<const float>(remoteStreamScratch.data(), frames));
                }
            }
        } else if (capturing) {
            // No room, so the friend's tracks record silence rather than
            // falling behind and shifting the timeline.
            recorder_.write(
                jamlink::record::RecordTrack::RemoteInstrument,
                std::span<const float>(remoteScratch.data(), frames));
            recorder_.write(
                jamlink::record::RecordTrack::RemoteVoice,
                std::span<const float>(remoteScratch.data(), frames));
        }

        BYTE* data = nullptr;
        result = output.render->GetBuffer(frames, &data);
        if (FAILED(result)) {
            state_.store(stateForFailure(result), std::memory_order_release);
            return false;
        }
        std::memset(data, 0, static_cast<std::size_t>(frames) * output.format.blockAlign);

        const float targetInstrumentGain = instrumentEnabled_.load(std::memory_order_acquire) != 0U
            ? instrumentGain_.load() : 0.0F;
        const float targetVoiceGain = voiceEnabled_.load(std::memory_order_acquire) != 0U
            ? voiceGain_.load() : 0.0F;
        const float instrumentStep = (targetInstrumentGain - currentInstrumentGain)
            / static_cast<float>(frames);
        const float voiceStep = (targetVoiceGain - currentVoiceGain)
            / static_cast<float>(frames);

        const std::uint32_t toneRequest = testToneRequest_.load(std::memory_order_acquire);
        if (toneRequest != observedToneRequest) {
            observedToneRequest = toneRequest;
            toneFramesRemaining = output.format.sampleRate;
            tonePhase = 0.0;
        }
        constexpr double twoPi = 6.28318530717958647692;
        const double toneStep = twoPi * 440.0 / static_cast<double>(output.format.sampleRate);
        for (UINT32 frame = 0U; frame < frames; ++frame) {
            currentInstrumentGain += instrumentStep;
            currentVoiceGain += voiceStep;
            float mixed = instrumentScratch[frame] * currentInstrumentGain
                + voiceScratch[frame] * currentVoiceGain
                + remoteScratch[frame];
            if (toneFramesRemaining > 0U) {
                mixed += static_cast<float>(std::sin(tonePhase) * 0.125);
                tonePhase += toneStep;
                if (tonePhase >= twoPi) {
                    tonePhase -= twoPi;
                }
                --toneFramesRemaining;
            }
            remoteNetworkScratch[frame] = mixed;
        }
        monitorMixHealth_.process(
            std::span<const float>(remoteNetworkScratch.data(), frames));
        float peak = 0.0F;
        for (UINT32 frame = 0U; frame < frames; ++frame) {
            const float mixed = std::clamp(
                remoteNetworkScratch[frame], -outputCeiling, outputCeiling);
            peak = std::max(peak, std::abs(mixed));
            encodeSample(data, output.format, frame, output.selectedPrimary, mixed);
            if (output.hasSecondary && output.selectedSecondary != output.selectedPrimary) {
                encodeSample(data, output.format, frame, output.selectedSecondary, mixed);
            }
        }
        currentInstrumentGain = targetInstrumentGain;
        currentVoiceGain = targetVoiceGain;
        outputPeak_.store(peak);
        underruns_.store(
            instrumentBuffer.underrunCount() + voiceBuffer.underrunCount(),
            std::memory_order_relaxed);
        overruns_.store(
            instrumentBuffer.overrunCount() + voiceBuffer.overrunCount(),
            std::memory_order_relaxed);
        result = output.render->ReleaseBuffer(frames, 0U);
        if (FAILED(result)) {
            state_.store(stateForFailure(result), std::memory_order_release);
            return false;
        }
        return true;
    }

    UniqueHandle stopEvent_;
    std::thread worker_;
    std::atomic<SoundcheckAudioState> state_{SoundcheckAudioState::Stopped};
    RealtimeAtomicFloat instrumentGain_{1.0F};
    RealtimeAtomicFloat voiceGain_{1.0F};
    std::atomic<std::uint32_t> instrumentEnabled_{1U};
    std::atomic<std::uint32_t> voiceEnabled_{1U};
    RealtimeAtomicFloat instrumentPeak_;
    RealtimeAtomicFloat voicePeak_;
    RealtimeAtomicFloat outputPeak_;
    LevelMeter instrumentInputHealth_{
        LevelMeter::nativeInputClipThreshold,
        LevelMeter::nativeInputRiskThreshold,
        LevelMeter::nativeInputRiskSampleCount};
    LevelMeter voiceInputHealth_{
        LevelMeter::nativeInputClipThreshold,
        LevelMeter::nativeInputRiskThreshold,
        LevelMeter::nativeInputRiskSampleCount};
    LevelMeter instrumentSendHealth_;
    LevelMeter voiceSendHealth_;
    LevelMeter monitorMixHealth_;
    LevelMeter recordingInstrumentHealth_;
    LevelMeter recordingVoiceHealth_;
    std::atomic<std::uint32_t> outputSampleRate_{0U};
    // Capture rates, which a local original is written at rather than being
    // converted to the timeline's.
    std::atomic<std::uint32_t> instrumentCaptureRate_{0U};
    std::atomic<std::uint32_t> voiceCaptureRate_{0U};
    std::atomic<std::uint32_t> outputBufferFrames_{0U};
    std::atomic<std::uint64_t> underruns_{0U};
    std::atomic<std::uint64_t> overruns_{0U};
    std::atomic<std::uint32_t> testToneRequest_{0U};
    std::atomic<std::uint32_t> tunerEnabled_{0U};
    std::atomic<std::int32_t> nativeError_{0};
    std::atomic<std::uint32_t> instrumentSourceClipped_{0U};
    std::atomic<std::uint32_t> voiceSourceClipped_{0U};
    InstrumentTuner tuner_;
    jamlink::record::SessionRecorder recorder_;
    jamlink::record::RecordTrackSelection recordSelection_{};
    jamlink::network::IPeerAudioExchange* peerExchange_{nullptr};
};

} // namespace

std::unique_ptr<ISoundcheckAudioService> createWasapiSoundcheckAudioService() {
    return std::make_unique<WasapiSoundcheckAudioService>();
}

} // namespace jamlink::audio
