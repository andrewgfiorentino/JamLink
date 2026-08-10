// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/audio_route_graph.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace jamlink::audio {

namespace {

constexpr std::size_t invalidBusIndex = std::numeric_limits<std::size_t>::max();

} // namespace

AudioRouteGraph::AudioRouteGraph(
    AudioGraphPurpose purpose,
    std::size_t maximumFrames,
    std::span<const AudioBusDefinition> buses,
    std::span<const AudioRouteDefinition> routes)
    : purpose_(purpose), maximumFrames_(maximumFrames) {
    if (maximumFrames_ == 0U || buses.empty()) {
        throw std::invalid_argument("AudioRouteGraph requires frames and buses");
    }

    buses_.reserve(buses.size());
    for (const auto& definition : buses) {
        if (definition.channelCount == 0U || findBusIndex(definition.id) != invalidBusIndex
            || maximumFrames_ > std::numeric_limits<std::size_t>::max()
                    / definition.channelCount) {
            throw std::invalid_argument("AudioRouteGraph bus definitions must be valid and unique");
        }
        if (purpose_ == AudioGraphPurpose::PrivateSoundcheck
            && (definition.role == AudioBusRole::NetworkSend
                || definition.role == AudioBusRole::NetworkReceive
                || definition.role == AudioBusRole::RemoteMusic
                || definition.role == AudioBusRole::RemoteVoice)) {
            throw std::invalid_argument(
                "Private Soundcheck graphs cannot contain network or remote buses");
        }
        buses_.push_back(BusStorage{
            definition,
            std::vector<float>(maximumFrames_ * definition.channelCount, 0.0F)});
    }

    routes_.reserve(routes.size());
    for (const auto& route : routes) {
        const std::size_t sourceIndex = findBusIndex(route.sourceBus);
        const std::size_t destinationIndex = findBusIndex(route.destinationBus);
        if (sourceIndex == invalidBusIndex || destinationIndex == invalidBusIndex
            || route.sourceChannel >= buses_[sourceIndex].definition.channelCount
            || route.destinationChannel >= buses_[destinationIndex].definition.channelCount
            || !std::isfinite(route.linearGain)) {
            throw std::invalid_argument("AudioRouteGraph route is invalid");
        }
        routes_.push_back(CompiledRoute{
            sourceIndex,
            route.sourceChannel,
            destinationIndex,
            route.destinationChannel,
            route.linearGain});
    }

    std::vector<std::size_t> incomingRouteCount(buses_.size(), 0U);
    std::vector<std::vector<std::size_t>> outgoingBuses(buses_.size());
    for (const auto& route : routes_) {
        ++incomingRouteCount[route.destinationBusIndex];
        outgoingBuses[route.sourceBusIndex].push_back(route.destinationBusIndex);
    }

    std::vector<std::size_t> topologicalOrder;
    topologicalOrder.reserve(buses_.size());
    for (std::size_t busIndex = 0; busIndex < buses_.size(); ++busIndex) {
        if (incomingRouteCount[busIndex] == 0U) {
            topologicalOrder.push_back(busIndex);
        }
    }

    for (std::size_t orderIndex = 0; orderIndex < topologicalOrder.size(); ++orderIndex) {
        const std::size_t sourceIndex = topologicalOrder[orderIndex];
        for (const std::size_t destinationIndex : outgoingBuses[sourceIndex]) {
            --incomingRouteCount[destinationIndex];
            if (incomingRouteCount[destinationIndex] == 0U) {
                topologicalOrder.push_back(destinationIndex);
            }
        }
    }

    if (topologicalOrder.size() != buses_.size()) {
        throw std::invalid_argument("AudioRouteGraph routes must not contain cycles");
    }

    std::vector<std::size_t> topologicalRank(buses_.size(), 0U);
    for (std::size_t rank = 0; rank < topologicalOrder.size(); ++rank) {
        topologicalRank[topologicalOrder[rank]] = rank;
    }
    std::stable_sort(routes_.begin(), routes_.end(), [&](const auto& left, const auto& right) {
        return topologicalRank[left.sourceBusIndex] < topologicalRank[right.sourceBusIndex];
    });
}

bool AudioRouteGraph::beginBlock(std::size_t frameCount) noexcept {
    currentFrames_ = std::min(frameCount, maximumFrames_);
    for (auto& busStorage : buses_) {
        const std::size_t activeSamples = currentFrames_ * busStorage.definition.channelCount;
        std::fill_n(busStorage.samples.begin(), activeSamples, 0.0F);
    }
    return frameCount <= maximumFrames_;
}

std::span<float> AudioRouteGraph::mutableBus(AudioBusId id) noexcept {
    const std::size_t busIndex = findBusIndex(id);
    if (busIndex == invalidBusIndex) {
        return {};
    }
    auto& storage = buses_[busIndex];
    return {storage.samples.data(), currentFrames_ * storage.definition.channelCount};
}

std::span<const float> AudioRouteGraph::bus(AudioBusId id) const noexcept {
    const std::size_t busIndex = findBusIndex(id);
    if (busIndex == invalidBusIndex) {
        return {};
    }
    const auto& storage = buses_[busIndex];
    return {storage.samples.data(), currentFrames_ * storage.definition.channelCount};
}

void AudioRouteGraph::processRoutes() noexcept {
    for (const auto& route : routes_) {
        const auto& source = buses_[route.sourceBusIndex];
        auto& destination = buses_[route.destinationBusIndex];
        const std::size_t sourceChannels = source.definition.channelCount;
        const std::size_t destinationChannels = destination.definition.channelCount;

        for (std::size_t frame = 0; frame < currentFrames_; ++frame) {
            const float sourceSample = source.samples[
                frame * sourceChannels + route.sourceChannel];
            const float contribution = std::isfinite(sourceSample)
                ? sourceSample * route.linearGain
                : 0.0F;
            float& destinationSample = destination.samples[
                frame * destinationChannels + route.destinationChannel];
            const float mixed = destinationSample + contribution;
            destinationSample = std::isfinite(mixed) ? mixed : 0.0F;
        }
    }
}

std::size_t AudioRouteGraph::findBusIndex(AudioBusId id) const noexcept {
    for (std::size_t index = 0; index < buses_.size(); ++index) {
        if (buses_[index].definition.id == id) {
            return index;
        }
    }
    return invalidBusIndex;
}

} // namespace jamlink::audio
