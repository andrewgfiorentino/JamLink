// SPDX-License-Identifier: GPL-3.0-or-later

#define NOMINMAX
#include <Windows.h>

#include "windows_audio_services.hpp"

#include "jamlink/audio/realtime_atomic.hpp"

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
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

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
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
    ComApartment() noexcept {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
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

enum class Encoding : std::uint8_t { Float32, Pcm16, Pcm24, Pcm32, Unsupported };

struct NativeFormat final {
    Encoding encoding{Encoding::Unsupported};
    std::uint32_t sampleRate{0U};
    std::uint16_t channels{0U};
    std::uint16_t blockAlign{0U};
    std::uint16_t bytesPerSample{0U};
};

[[nodiscard]] NativeFormat describe(const WAVEFORMATEX& format) noexcept {
    NativeFormat result;
    result.sampleRate = format.nSamplesPerSec;
    result.channels = format.nChannels;
    result.blockAlign = format.nBlockAlign;
    result.bytesPerSample = static_cast<std::uint16_t>(format.wBitsPerSample / 8U);
    WORD tag = format.wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE
        && format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        if (extensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (extensible.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) {
            tag = WAVE_FORMAT_PCM;
        }
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && format.wBitsPerSample == 32U) {
        result.encoding = Encoding::Float32;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 16U) {
        result.encoding = Encoding::Pcm16;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 24U) {
        result.encoding = Encoding::Pcm24;
    } else if (tag == WAVE_FORMAT_PCM && format.wBitsPerSample == 32U) {
        result.encoding = Encoding::Pcm32;
    }
    return result;
}

[[nodiscard]] float decode(
    const BYTE* data,
    const NativeFormat& format,
    std::uint32_t frame,
    std::uint32_t channel) noexcept {
    const auto* sample = data + static_cast<std::size_t>(frame) * format.blockAlign
        + static_cast<std::size_t>(channel) * format.bytesPerSample;
    switch (format.encoding) {
    case Encoding::Float32: {
        float value = 0.0F;
        std::memcpy(&value, sample, sizeof(value));
        return std::isfinite(value) ? value : 0.0F;
    }
    case Encoding::Pcm16: {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<float>(value) / 32'768.0F;
    }
    case Encoding::Pcm24: {
        std::int32_t value = static_cast<std::int32_t>(sample[0])
            | (static_cast<std::int32_t>(sample[1]) << 8)
            | (static_cast<std::int32_t>(sample[2]) << 16);
        if ((value & 0x0080'0000) != 0) {
            value |= static_cast<std::int32_t>(0xFF00'0000U);
        }
        return static_cast<float>(value) / 8'388'608.0F;
    }
    case Encoding::Pcm32: {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<float>(static_cast<double>(value) / 2'147'483'648.0);
    }
    case Encoding::Unsupported:
        return 0.0F;
    }
    return 0.0F;
}

[[nodiscard]] std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    static_cast<void>(MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        result.data(), size));
    return result;
}

struct CaptureStream final {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    UniqueWaveFormat waveFormat;
    NativeFormat format;
    UniqueHandle event;
    UINT32 bufferFrames{0U};
    std::uint32_t channel{0U};
};

[[nodiscard]] HRESULT initializeCapture(
    const SoundcheckEndpointOption& option,
    CaptureStream& stream) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    HRESULT result = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (FAILED(result)) return result;
    const std::wstring endpoint = widen(option.endpointId);
    if (endpoint.empty()) return E_INVALIDARG;
    ComPtr<IMMDevice> device;
    result = enumerator->GetDevice(endpoint.c_str(), &device);
    if (FAILED(result)) return result;
    result = device->Activate(
        __uuidof(IAudioClient), CLSCTX_ALL, nullptr,
        reinterpret_cast<void**>(stream.client.GetAddressOf()));
    if (FAILED(result)) return result;
    WAVEFORMATEX* allocated = nullptr;
    result = stream.client->GetMixFormat(&allocated);
    if (FAILED(result) || allocated == nullptr) {
        CoTaskMemFree(allocated);
        return FAILED(result) ? result : E_FAIL;
    }
    stream.waveFormat.reset(allocated);
    stream.format = describe(*stream.waveFormat);
    if (stream.format.encoding == Encoding::Unsupported
        || option.primaryChannel >= stream.format.channels) {
        return AUDCLNT_E_UNSUPPORTED_FORMAT;
    }
    constexpr DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    ComPtr<IAudioClient3> client3;
    if (SUCCEEDED(stream.client.As(&client3))) {
        UINT32 defaults = 0U;
        UINT32 fundamental = 0U;
        UINT32 minimum = 0U;
        UINT32 maximum = 0U;
        result = client3->GetSharedModeEnginePeriod(
            stream.waveFormat.get(), &defaults, &fundamental, &minimum, &maximum);
        if (SUCCEEDED(result)) {
            result = client3->InitializeSharedAudioStream(
                flags, minimum, stream.waveFormat.get(), nullptr);
        }
    } else {
        result = stream.client->Initialize(
            AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, stream.waveFormat.get(), nullptr);
    }
    if (FAILED(result)) return result;
    stream.event.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!stream.event) return HRESULT_FROM_WIN32(GetLastError());
    result = stream.client->SetEventHandle(stream.event.get());
    if (FAILED(result)) return result;
    result = stream.client->GetBufferSize(&stream.bufferFrames);
    if (FAILED(result)) return result;
    result = stream.client->GetService(IID_PPV_ARGS(&stream.capture));
    if (FAILED(result)) return result;
    stream.channel = option.primaryChannel;
    return S_OK;
}

[[nodiscard]] SoundcheckAudioState failureState(HRESULT error) noexcept {
    if (error == AUDCLNT_E_DEVICE_INVALIDATED || error == AUDCLNT_E_RESOURCES_INVALIDATED) {
        return SoundcheckAudioState::DeviceInvalidated;
    }
    if (error == AUDCLNT_E_UNSUPPORTED_FORMAT) {
        return SoundcheckAudioState::UnsupportedFormat;
    }
    if (error == E_NOTFOUND || error == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        return SoundcheckAudioState::DeviceUnavailable;
    }
    return SoundcheckAudioState::InitializationFailed;
}

class SecondaryWasapiCapture final : public ISecondaryWasapiCapture {
public:
    ~SecondaryWasapiCapture() override { stop(); }

    [[nodiscard]] bool prepare(
        const SoundcheckEndpointOption& option,
        HybridClockBridge& destination) override {
        stop();
        if (option.backend != SoundcheckBackend::WasapiShared || option.endpointId.empty()) {
            state_.store(SoundcheckAudioState::UnsupportedFormat, std::memory_order_release);
            return false;
        }
        stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        beginEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!stopEvent_ || !beginEvent_) {
            state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
            return false;
        }
        destination_ = &destination;
        state_.store(SoundcheckAudioState::Starting, std::memory_order_release);
        std::promise<bool> prepared;
        auto future = prepared.get_future();
        worker_ = std::thread(
            [this, option, ready = std::move(prepared)]() mutable {
                run(option, std::move(ready));
            });
        const bool result = future.get();
        if (!result) {
            stop();
        }
        return result;
    }

    [[nodiscard]] bool begin() noexcept override {
        if (!beginEvent_ || !SetEvent(beginEvent_.get())) return false;
        for (std::size_t attempt = 0U; attempt < 1'000U; ++attempt) {
            const auto state = state_.load(std::memory_order_acquire);
            if (state == SoundcheckAudioState::Running) return true;
            if (state != SoundcheckAudioState::Starting) return false;
            Sleep(1U);
        }
        return false;
    }

    void stop() noexcept override {
        if (stopEvent_) static_cast<void>(SetEvent(stopEvent_.get()));
        if (worker_.joinable()) worker_.join();
        stopEvent_.reset();
        beginEvent_.reset();
        destination_ = nullptr;
        if (state_.load(std::memory_order_acquire) == SoundcheckAudioState::Running
            || state_.load(std::memory_order_acquire) == SoundcheckAudioState::Starting
            || state_.load(std::memory_order_acquire) == SoundcheckAudioState::DeviceInvalidated) {
            state_.store(SoundcheckAudioState::Stopped, std::memory_order_release);
        }
    }

    void clearSignalHealth() noexcept override {
        inputHealth_.clearClipLatch();
        inputClipped_.store(0U, std::memory_order_release);
    }

    [[nodiscard]] bool inputClipped() const noexcept override {
        return inputClipped_.load(std::memory_order_acquire) != 0U;
    }

    [[nodiscard]] SecondaryCaptureTelemetry telemetry() const noexcept override {
        return SecondaryCaptureTelemetry{
            state_.load(std::memory_order_acquire),
            sampleRate_.load(std::memory_order_relaxed),
            peak_.load(),
            nativeError_.load(std::memory_order_relaxed),
            signalHealthTelemetry(inputHealth_.snapshot())};
    }

private:
    [[nodiscard]] bool process(CaptureStream& stream, std::vector<float>& scratch) noexcept {
        UINT32 packetFrames = 0U;
        HRESULT result = stream.capture->GetNextPacketSize(&packetFrames);
        while (SUCCEEDED(result) && packetFrames != 0U) {
            BYTE* data = nullptr;
            UINT32 frames = 0U;
            DWORD flags = 0U;
            result = stream.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(result)) {
                nativeError_.store(static_cast<std::int32_t>(result), std::memory_order_relaxed);
                return false;
            }
            if (frames > scratch.size()) {
                static_cast<void>(stream.capture->ReleaseBuffer(frames));
                nativeError_.store(E_BOUNDS, std::memory_order_relaxed);
                return false;
            }
            float peak = 0.0F;
            for (UINT32 frame = 0U; frame < frames; ++frame) {
                const float sample = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0U
                    ? 0.0F : decode(data, stream.format, frame, stream.channel);
                scratch[frame] = sample;
                peak = std::max(peak, std::abs(sample));
            }
            peak_.store(std::clamp(peak, 0.0F, 1.0F));
            if (peak >= LevelMeter::nativeInputClipThreshold) {
                inputClipped_.store(1U, std::memory_order_release);
            }
            inputHealth_.process(std::span<const float>(scratch.data(), frames));
            if (destination_ != nullptr) {
                static_cast<void>(destination_->push(
                    std::span<const float>(scratch.data(), frames)));
            }
            result = stream.capture->ReleaseBuffer(frames);
            if (FAILED(result)) {
                nativeError_.store(static_cast<std::int32_t>(result), std::memory_order_relaxed);
                return false;
            }
            result = stream.capture->GetNextPacketSize(&packetFrames);
        }
        if (FAILED(result)) {
            nativeError_.store(static_cast<std::int32_t>(result), std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void run(const SoundcheckEndpointOption& option, std::promise<bool> prepared) noexcept {
        ComApartment apartment;
        if (!apartment.available()) {
            state_.store(SoundcheckAudioState::InitializationFailed, std::memory_order_release);
            prepared.set_value(false);
            return;
        }
        DWORD taskIndex = 0U;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        CaptureStream stream;
        HRESULT result = initializeCapture(option, stream);
        if (FAILED(result)) {
            nativeError_.store(static_cast<std::int32_t>(result), std::memory_order_relaxed);
            state_.store(failureState(result), std::memory_order_release);
            prepared.set_value(false);
            if (mmcss != nullptr) static_cast<void>(AvRevertMmThreadCharacteristics(mmcss));
            return;
        }
        const std::uint32_t expectedRate = stream.format.sampleRate;
        sampleRate_.store(expectedRate, std::memory_order_relaxed);
        std::vector<float> scratch(stream.bufferFrames, 0.0F);
        prepared.set_value(true);
        const HANDLE prepareWaits[]{stopEvent_.get(), beginEvent_.get()};
        if (WaitForMultipleObjects(2U, prepareWaits, FALSE, INFINITE) != WAIT_OBJECT_0 + 1U) {
            if (mmcss != nullptr) static_cast<void>(AvRevertMmThreadCharacteristics(mmcss));
            return;
        }

        bool keepRunning = true;
        while (keepRunning) {
            result = stream.client->Start();
            if (FAILED(result)) {
                nativeError_.store(static_cast<std::int32_t>(result), std::memory_order_relaxed);
                state_.store(failureState(result), std::memory_order_release);
            } else {
                state_.store(SoundcheckAudioState::Running, std::memory_order_release);
                const HANDLE waits[]{stopEvent_.get(), stream.event.get()};
                while (keepRunning) {
                    const DWORD wait = WaitForMultipleObjects(2U, waits, FALSE, INFINITE);
                    if (wait == WAIT_OBJECT_0) {
                        keepRunning = false;
                    } else if (wait == WAIT_OBJECT_0 + 1U) {
                        if (!process(stream, scratch)) {
                            state_.store(SoundcheckAudioState::DeviceInvalidated,
                                         std::memory_order_release);
                            break;
                        }
                    } else {
                        state_.store(SoundcheckAudioState::DeviceInvalidated,
                                     std::memory_order_release);
                        break;
                    }
                }
                static_cast<void>(stream.client->Stop());
            }
            if (!keepRunning) break;

            peak_.store(0.0F);
            if (WaitForSingleObject(stopEvent_.get(), 1'000U) == WAIT_OBJECT_0) break;
            CaptureStream replacement;
            result = initializeCapture(option, replacement);
            if (FAILED(result) || replacement.format.sampleRate != expectedRate) {
                nativeError_.store(static_cast<std::int32_t>(
                    FAILED(result) ? result : AUDCLNT_E_UNSUPPORTED_FORMAT),
                    std::memory_order_relaxed);
                state_.store(FAILED(result) ? failureState(result)
                                            : SoundcheckAudioState::UnsupportedFormat,
                             std::memory_order_release);
                continue;
            }
            stream = std::move(replacement);
            scratch.assign(stream.bufferFrames, 0.0F);
        }
        if (state_.load(std::memory_order_acquire) == SoundcheckAudioState::Running) {
            state_.store(SoundcheckAudioState::Stopped, std::memory_order_release);
        }
        if (mmcss != nullptr) static_cast<void>(AvRevertMmThreadCharacteristics(mmcss));
    }

    UniqueHandle stopEvent_;
    UniqueHandle beginEvent_;
    std::thread worker_;
    HybridClockBridge* destination_{nullptr};
    std::atomic<SoundcheckAudioState> state_{SoundcheckAudioState::Stopped};
    std::atomic<std::uint32_t> sampleRate_{0U};
    RealtimeAtomicFloat peak_;
    std::atomic<std::int32_t> nativeError_{0};
    std::atomic<std::uint32_t> inputClipped_{0U};
    LevelMeter inputHealth_{LevelMeter::nativeInputClipThreshold};
};

} // namespace

std::unique_ptr<ISecondaryWasapiCapture> createSecondaryWasapiCapture() {
    return std::make_unique<SecondaryWasapiCapture>();
}

} // namespace jamlink::audio
