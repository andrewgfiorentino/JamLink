// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/device_backend.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace jamlink::tests {

class SimulatedAudioDeviceBackend final : public audio::IAudioDeviceBackend {
public:
    explicit SimulatedAudioDeviceBackend(std::vector<audio::AudioDeviceInfo> devices)
        : devices_() {
        devices_.reserve(devices.size());
        for (auto& device : devices) {
            devices_.push_back(DeviceState{std::move(device), true});
        }
    }

    [[nodiscard]] audio::AudioBackendKind kind() const noexcept override {
        return audio::AudioBackendKind::Test;
    }

    [[nodiscard]] std::vector<audio::AudioDeviceInfo> enumerateDevices() override {
        std::vector<audio::AudioDeviceInfo> available;
        for (const auto& device : devices_) {
            if (device.present) {
                available.push_back(device.info);
            }
        }
        return available;
    }

    [[nodiscard]] audio::AudioOpenResult open(
        const audio::AudioStreamConfiguration& configuration,
        audio::IAudioProcessCallback& callback) override {
        const auto* device = find(configuration.deviceId);
        if (device == nullptr || !device->present) {
            return {false, "Simulated device is not present"};
        }
        if (!supportsSampleRate(device->info, configuration.sampleRate)
            || !supportsBufferSize(device->info.bufferCapabilities, configuration.bufferFrames)
            || !channelsExist(device->info.inputChannels, configuration.inputChannelIndices)
            || !channelsExist(device->info.outputChannels, configuration.outputChannelIndices)) {
            return {false, "Simulated stream configuration is unsupported"};
        }

        configuration_ = configuration;
        callback_ = &callback;
        input_.assign(
            static_cast<std::size_t>(configuration.bufferFrames)
                * configuration.inputChannelIndices.size(),
            0.0F);
        output_.assign(
            static_cast<std::size_t>(configuration.bufferFrames)
                * configuration.outputChannelIndices.size(),
            0.0F);
        opened_ = true;
        running_ = false;
        framePosition_ = 0U;
        return {
            true,
            {},
            audio::OpenedAudioStreamInfo{
                configuration.sampleRate,
                configuration.bufferFrames,
                device->info.latencyCapabilities.reportedInputFrames,
                device->info.latencyCapabilities.reportedOutputFrames}};
    }

    bool start() override {
        if (!opened_ || callback_ == nullptr || !activeDevicePresent()) {
            return false;
        }
        running_ = true;
        return true;
    }

    void stop() noexcept override { running_ = false; }

    void close() noexcept override {
        running_ = false;
        opened_ = false;
        callback_ = nullptr;
        framePosition_ = 0U;
    }

    // Control-thread test action that simulates hot-plug state.
    void setPresent(const std::string& stableId, bool present) noexcept {
        if (auto* device = find(stableId)) {
            device->present = present;
            if (!present && configuration_.deviceId == stableId) {
                running_ = false;
                opened_ = false;
                callback_ = nullptr;
            }
        }
    }

    // Simulated device-callback action. Storage was allocated by open().
    [[nodiscard]] bool processOneBlock(std::span<const float> input) noexcept {
        if (!running_ || callback_ == nullptr || input.size() != input_.size()) {
            return false;
        }
        std::copy(input.begin(), input.end(), input_.begin());
        std::fill(output_.begin(), output_.end(), 0.0F);
        callback_->process(audio::AudioProcessBlock{
            {input_, configuration_.inputChannelIndices.size()},
            {output_, configuration_.outputChannelIndices.size()},
            framePosition_});
        framePosition_ += configuration_.bufferFrames;
        return true;
    }

    [[nodiscard]] std::span<const float> lastOutput() const noexcept { return output_; }

private:
    struct DeviceState final {
        audio::AudioDeviceInfo info;
        bool present{true};
    };

    [[nodiscard]] DeviceState* find(const std::string& stableId) noexcept {
        const auto match = std::find_if(devices_.begin(), devices_.end(), [&](DeviceState& device) {
            return device.info.stableId == stableId;
        });
        return match == devices_.end() ? nullptr : &*match;
    }

    [[nodiscard]] const DeviceState* find(const std::string& stableId) const noexcept {
        const auto match = std::find_if(
            devices_.begin(), devices_.end(), [&](const DeviceState& device) {
                return device.info.stableId == stableId;
            });
        return match == devices_.end() ? nullptr : &*match;
    }

    [[nodiscard]] bool activeDevicePresent() const noexcept {
        const auto* device = find(configuration_.deviceId);
        return device != nullptr && device->present;
    }

    [[nodiscard]] static bool channelsExist(
        const std::vector<audio::AudioChannelInfo>& available,
        const std::vector<std::uint32_t>& selected) noexcept {
        for (std::size_t left = 0; left < selected.size(); ++left) {
            for (std::size_t right = left + 1U; right < selected.size(); ++right) {
                if (selected[left] == selected[right]) {
                    return false;
                }
            }
        }
        return std::all_of(selected.begin(), selected.end(), [&](std::uint32_t selectedIndex) {
            return std::any_of(available.begin(), available.end(), [&](const auto& channel) {
                return channel.index == selectedIndex;
            });
        });
    }

    [[nodiscard]] static bool supportsSampleRate(
        const audio::AudioDeviceInfo& device,
        std::uint32_t sampleRate) noexcept {
        return sampleRate != 0U
            && (device.supportedSampleRates.empty()
                || std::find(
                    device.supportedSampleRates.begin(),
                    device.supportedSampleRates.end(),
                    sampleRate) != device.supportedSampleRates.end());
    }

    [[nodiscard]] static bool supportsBufferSize(
        const audio::AudioBufferCapabilities& capabilities,
        std::uint32_t frames) noexcept {
        if (frames == 0U) {
            return false;
        }
        if (!capabilities.discreteFrameCounts.empty()) {
            return std::find(
                capabilities.discreteFrameCounts.begin(),
                capabilities.discreteFrameCounts.end(),
                frames) != capabilities.discreteFrameCounts.end();
        }
        if (capabilities.minimumFrames != 0U && frames < capabilities.minimumFrames) {
            return false;
        }
        if (capabilities.maximumFrames != 0U && frames > capabilities.maximumFrames) {
            return false;
        }
        return capabilities.frameGranularity == 0U
            || capabilities.minimumFrames == 0U
            || (frames - capabilities.minimumFrames) % capabilities.frameGranularity == 0U;
    }

    std::vector<DeviceState> devices_;
    audio::AudioStreamConfiguration configuration_;
    audio::IAudioProcessCallback* callback_{nullptr};
    std::vector<float> input_;
    std::vector<float> output_;
    std::uint64_t framePosition_{0};
    bool opened_{false};
    bool running_{false};
};

} // namespace jamlink::tests
