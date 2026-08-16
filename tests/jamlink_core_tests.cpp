// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/audio_route_graph.hpp"
#include "jamlink/audio/async_mono_resampler.hpp"
#include "jamlink/audio/gain_stage.hpp"
#include "jamlink/audio/hybrid_clock_bridge.hpp"
#include "jamlink/audio/level_meter.hpp"
#include "jamlink/audio/native_sample_conversion.hpp"
#include "jamlink/audio/private_soundcheck_processor.hpp"
#include "jamlink/audio/spsc_audio_ring.hpp"
#include "jamlink/clock/clock_domain_controller.hpp"
#include "jamlink/control/readiness_tracker.hpp"
#include "jamlink/network/connection_preflight.hpp"
#include "jamlink/preferences/preferences_store.hpp"
#include "support/simulated_audio_device_backend.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_MSC_VER)
#include <malloc.h>
#endif

namespace {

std::atomic<bool> allocationTrackingEnabled{false};
std::atomic<std::size_t> trackedAllocationCount{0};

} // namespace

void* operator new(std::size_t size) {
    if (allocationTrackingEnabled.load(std::memory_order_relaxed)) {
        trackedAllocationCount.fetch_add(1U, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    return ::operator new(size);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete[](void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    if (allocationTrackingEnabled.load(std::memory_order_relaxed)) {
        trackedAllocationCount.fetch_add(1U, std::memory_order_relaxed);
    }
    const auto byteAlignment = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    void* memory = _aligned_malloc(size, byteAlignment);
#else
    const std::size_t alignedSize = ((size + byteAlignment - 1U) / byteAlignment) * byteAlignment;
    void* memory = std::aligned_alloc(byteAlignment, alignedSize);
#endif
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}

void operator delete(void* memory, std::align_val_t) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

void operator delete[](void* memory, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete(void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

void operator delete[](void* memory, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(memory, alignment);
}

namespace {

using TestFunction = std::function<void()>;

struct TestCase final {
    std::string name;
    TestFunction function;
};

std::vector<TestCase>& tests() {
    static std::vector<TestCase> allTests;
    return allTests;
}

struct RegisterTest final {
    RegisterTest(std::string name, TestFunction function) {
        tests().push_back(TestCase{std::move(name), std::move(function)});
    }
};

[[noreturn]] void fail(const char* expression, const char* file, int line) {
    throw std::runtime_error(
        std::string(file) + ':' + std::to_string(line) + " expectation failed: " + expression);
}

bool near(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

#define JAMLINK_TEST(name) \
    void name(); \
    const RegisterTest register_##name(#name, name); \
    void name()

#define EXPECT_TRUE(expression) \
    do { if (!(expression)) { fail(#expression, __FILE__, __LINE__); } } while (false)

#define EXPECT_NEAR(actual, expected, tolerance) \
    do { if (!near((actual), (expected), (tolerance))) { \
        fail(#actual " ~= " #expected, __FILE__, __LINE__); \
    } } while (false)

JAMLINK_TEST(connection_preflight_classifies_deterministic_fake_outcomes) {
    using namespace jamlink::network;
    const ConnectionPreflightChecks ready{
        true,
        true,
        true,
        true,
        PublicAddressDiscoveryState::Succeeded,
        PortMappingState::Succeeded,
        ReachabilityAssessment::LikelyReachable};

    const auto readyResult = evaluateConnectionPreflight(ready);
    EXPECT_TRUE(readyResult.outcome == ConnectionPreflightOutcome::Ready);
    EXPECT_TRUE(readyResult.action == ConnectionPreflightAction::None);
    EXPECT_TRUE(readyResult.directInviteAllowed);

    auto simulated = ready;
    simulated.audioReady = false;
    auto result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.outcome == ConnectionPreflightOutcome::Blocked);
    EXPECT_TRUE(result.action == ConnectionPreflightAction::FinishSoundCheck);
    EXPECT_TRUE(!result.directInviteAllowed);

    simulated = ready;
    simulated.buildIdentityReady = false;
    result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.action == ConnectionPreflightAction::UseCurrentBuild);
    EXPECT_TRUE(!result.directInviteAllowed);

    simulated = ready;
    simulated.protocolIdentityReady = false;
    result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.action == ConnectionPreflightAction::UseCurrentBuild);

    simulated = ready;
    simulated.udpBindSucceeded = false;
    result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.action == ConnectionPreflightAction::ChooseAnotherUdpPort);
    EXPECT_TRUE(!result.directInviteAllowed);

    // A router that was asked for a port and refused it. Port forwarding is a
    // real remedy but not one most musicians can follow, and with a single
    // invite code only the host has to be reachable, so the instruction that
    // actually resolves the session is to let the other person host.
    simulated = ready;
    simulated.publicAddress = PublicAddressDiscoveryState::Failed;
    simulated.portMapping = PortMappingState::Failed;
    simulated.reachability = ReachabilityAssessment::Unknown;
    result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.outcome == ConnectionPreflightOutcome::DirectMayNeedHelp);
    EXPECT_TRUE(result.action == ConnectionPreflightAction::AskFriendToHost);
    // The invite is still offered: a mapping refusal is not proof of
    // unreachability, and the port may already be forwarded by hand.
    EXPECT_TRUE(result.directInviteAllowed);

    // Automatic mapping switched off is a different situation from a refusal,
    // and the useful instruction is to turn it back on.
    simulated = ready;
    simulated.portMapping = PortMappingState::NotRequested;
    simulated.reachability = ReachabilityAssessment::Unknown;
    result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.outcome == ConnectionPreflightOutcome::DirectMayNeedHelp);
    EXPECT_TRUE(result.action
        == ConnectionPreflightAction::EnableMappingOrForwardPort);
    EXPECT_TRUE(result.directInviteAllowed);

    simulated = ready;
    simulated.reachability = ReachabilityAssessment::LikelyBlocked;
    result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.outcome == ConnectionPreflightOutcome::DirectMayNeedHelp);
    EXPECT_TRUE(result.action == ConnectionPreflightAction::CheckFirewall);
    EXPECT_TRUE(result.directInviteAllowed);

    // A router that rewrites the external port cannot be reached directly and
    // this build has no relay, so reporting "relay required" left a musician
    // with nothing they could do. Only hosting is lost; joining still works.
    simulated = ready;
    simulated.reachability = ReachabilityAssessment::RelayRequired;
    result = evaluateConnectionPreflight(simulated);
    EXPECT_TRUE(result.outcome == ConnectionPreflightOutcome::JoinOnly);
    EXPECT_TRUE(result.action == ConnectionPreflightAction::AskFriendToHost);
    // An invite from this machine genuinely cannot work, so it is not offered.
    EXPECT_TRUE(!result.directInviteAllowed);
}

JAMLINK_TEST(spsc_ring_preserves_order_across_wrap) {
    jamlink::audio::SpscAudioRing ring(8U, 1U);
    const std::array first{0.0F, 1.0F, 2.0F, 3.0F, 4.0F, 5.0F};
    EXPECT_TRUE(ring.write(first) == first.size());

    std::array<float, 4> head{};
    EXPECT_TRUE(ring.readAndZeroFill(head) == head.size());
    EXPECT_TRUE(std::equal(head.begin(), head.end(), first.begin()));

    const std::array second{6.0F, 7.0F, 8.0F, 9.0F, 10.0F, 11.0F};
    EXPECT_TRUE(ring.write(second) == second.size());

    std::array<float, 8> tail{};
    EXPECT_TRUE(ring.readAndZeroFill(tail) == tail.size());
    const std::array expected{4.0F, 5.0F, 6.0F, 7.0F, 8.0F, 9.0F, 10.0F, 11.0F};
    EXPECT_TRUE(tail == expected);
    EXPECT_TRUE(ring.underrunCount() == 0U);
    EXPECT_TRUE(ring.overrunCount() == 0U);
}

JAMLINK_TEST(spsc_ring_zero_fills_underflow) {
    jamlink::audio::SpscAudioRing ring(8U, 2U);
    const std::array input{1.0F, -1.0F, 0.5F, -0.5F};
    EXPECT_TRUE(ring.write(input) == 2U);

    std::array<float, 8> output{};
    EXPECT_TRUE(ring.readAndZeroFill(output) == 2U);
    EXPECT_TRUE(output[0] == 1.0F && output[1] == -1.0F);
    EXPECT_TRUE(output[2] == 0.5F && output[3] == -0.5F);
    EXPECT_TRUE(std::all_of(output.begin() + 4, output.end(), [](float value) {
        return value == 0.0F;
    }));
    EXPECT_TRUE(ring.underrunCount() == 1U);
}

JAMLINK_TEST(spsc_ring_preserves_sequence_with_concurrent_producer_consumer) {
    constexpr std::size_t framesPerTransfer = 8U;
    constexpr std::size_t transferCount = 10'000U;
    jamlink::audio::SpscAudioRing ring(256U, 2U);
    std::atomic<bool> sequenceValid{true};

    std::thread producer([&] {
        std::array<float, framesPerTransfer * 2U> block{};
        for (std::size_t transfer = 0; transfer < transferCount; ++transfer) {
            for (std::size_t frame = 0; frame < framesPerTransfer; ++frame) {
                const auto sequence = static_cast<float>(transfer * framesPerTransfer + frame);
                block[frame * 2U] = sequence;
                block[frame * 2U + 1U] = -sequence;
            }
            while (ring.availableWriteFrames() < framesPerTransfer) {
                std::this_thread::yield();
            }
            if (ring.write(block) != framesPerTransfer) {
                sequenceValid.store(false, std::memory_order_relaxed);
                return;
            }
        }
    });

    std::thread consumer([&] {
        std::array<float, framesPerTransfer * 2U> block{};
        for (std::size_t transfer = 0; transfer < transferCount; ++transfer) {
            while (ring.availableReadFrames() < framesPerTransfer) {
                std::this_thread::yield();
            }
            if (ring.readAndZeroFill(block) != framesPerTransfer) {
                sequenceValid.store(false, std::memory_order_relaxed);
                return;
            }
            for (std::size_t frame = 0; frame < framesPerTransfer; ++frame) {
                const auto expected = static_cast<float>(transfer * framesPerTransfer + frame);
                if (block[frame * 2U] != expected || block[frame * 2U + 1U] != -expected) {
                    sequenceValid.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        }
    });

    producer.join();
    consumer.join();
    EXPECT_TRUE(sequenceValid.load(std::memory_order_relaxed));
    EXPECT_TRUE(ring.availableReadFrames() == 0U);
    EXPECT_TRUE(ring.underrunCount() == 0U);
    EXPECT_TRUE(ring.overrunCount() == 0U);
}

JAMLINK_TEST(route_graph_preserves_channels_and_fans_out) {
    constexpr jamlink::audio::AudioBusId music = 1U;
    constexpr jamlink::audio::AudioBusId monitor = 2U;
    constexpr jamlink::audio::AudioBusId record = 3U;
    constexpr jamlink::audio::AudioBusId networkSend = 4U;
    const std::array buses{
        jamlink::audio::AudioBusDefinition{music, jamlink::audio::AudioBusRole::LocalMusic, 2U},
        jamlink::audio::AudioBusDefinition{monitor, jamlink::audio::AudioBusRole::Monitor, 2U},
        jamlink::audio::AudioBusDefinition{record, jamlink::audio::AudioBusRole::Record, 2U},
        jamlink::audio::AudioBusDefinition{networkSend, jamlink::audio::AudioBusRole::NetworkSend, 2U}};
    const std::array routes{
        jamlink::audio::AudioRouteDefinition{music, 0U, monitor, 0U, 1.0F},
        jamlink::audio::AudioRouteDefinition{music, 1U, monitor, 1U, 1.0F},
        jamlink::audio::AudioRouteDefinition{music, 0U, record, 0U, 1.0F},
        jamlink::audio::AudioRouteDefinition{music, 1U, record, 1U, 1.0F},
        jamlink::audio::AudioRouteDefinition{music, 0U, networkSend, 0U, 1.0F},
        jamlink::audio::AudioRouteDefinition{music, 1U, networkSend, 1U, 1.0F}};
    jamlink::audio::AudioRouteGraph graph(
        jamlink::audio::AudioGraphPurpose::Session, 4U, buses, routes);
    EXPECT_TRUE(graph.beginBlock(4U));
    const std::array source{1.0F, -1.0F, 0.5F, -0.5F, 0.25F, -0.25F, 0.0F, -0.0F};
    std::ranges::copy(source, graph.mutableBus(music).begin());
    graph.processRoutes();

    EXPECT_TRUE(std::ranges::equal(graph.bus(monitor), source));
    EXPECT_TRUE(std::ranges::equal(graph.bus(record), source));
    EXPECT_TRUE(std::ranges::equal(graph.bus(networkSend), source));
}

JAMLINK_TEST(route_graph_orders_chains_and_rejects_cycles) {
    constexpr jamlink::audio::AudioBusId input = 11U;
    constexpr jamlink::audio::AudioBusId intermediate = 12U;
    constexpr jamlink::audio::AudioBusId output = 13U;
    const std::array buses{
        jamlink::audio::AudioBusDefinition{
            input, jamlink::audio::AudioBusRole::HardwareInput, 1U},
        jamlink::audio::AudioBusDefinition{
            intermediate, jamlink::audio::AudioBusRole::LocalMusic, 1U},
        jamlink::audio::AudioBusDefinition{
            output, jamlink::audio::AudioBusRole::Monitor, 1U}};
    const std::array reversedRoutes{
        jamlink::audio::AudioRouteDefinition{intermediate, 0U, output, 0U, 1.0F},
        jamlink::audio::AudioRouteDefinition{input, 0U, intermediate, 0U, 1.0F}};
    jamlink::audio::AudioRouteGraph graph(
        jamlink::audio::AudioGraphPurpose::Session, 3U, buses, reversedRoutes);
    EXPECT_TRUE(graph.beginBlock(3U));
    const std::array samples{1.0F, -0.5F, 0.25F};
    std::ranges::copy(samples, graph.mutableBus(input).begin());
    graph.processRoutes();
    EXPECT_TRUE(std::ranges::equal(graph.bus(output), samples));

    const std::array cyclicRoutes{
        jamlink::audio::AudioRouteDefinition{input, 0U, intermediate, 0U, 1.0F},
        jamlink::audio::AudioRouteDefinition{intermediate, 0U, input, 0U, 1.0F}};
    bool rejectedCycle = false;
    try {
        const jamlink::audio::AudioRouteGraph invalidGraph(
            jamlink::audio::AudioGraphPurpose::Session, 3U, buses, cyclicRoutes);
        static_cast<void>(invalidGraph);
    } catch (const std::invalid_argument&) {
        rejectedCycle = true;
    }
    EXPECT_TRUE(rejectedCycle);
}

JAMLINK_TEST(route_graph_rejects_sample_shape_overflow) {
    const std::array buses{
        jamlink::audio::AudioBusDefinition{
            17U, jamlink::audio::AudioBusRole::LocalMusic, 2U}};
    bool rejected = false;
    try {
        const jamlink::audio::AudioRouteGraph invalidGraph(
            jamlink::audio::AudioGraphPurpose::PrivateSoundcheck,
            std::numeric_limits<std::size_t>::max(),
            buses,
            std::span<const jamlink::audio::AudioRouteDefinition>{});
        static_cast<void>(invalidGraph);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

JAMLINK_TEST(private_soundcheck_graph_rejects_every_network_bus) {
    constexpr jamlink::audio::AudioBusId local = 21U;
    constexpr jamlink::audio::AudioBusId network = 22U;
    const auto expectRejectedRole = [&](jamlink::audio::AudioBusRole role) {
        const std::array buses{
            jamlink::audio::AudioBusDefinition{
                local, jamlink::audio::AudioBusRole::LocalMusic, 1U},
            jamlink::audio::AudioBusDefinition{network, role, 1U}};
        const std::array routes{
            jamlink::audio::AudioRouteDefinition{local, 0U, network, 0U, 1.0F}};
        bool rejected = false;
        try {
            const jamlink::audio::AudioRouteGraph invalidGraph(
                jamlink::audio::AudioGraphPurpose::PrivateSoundcheck,
                16U,
                buses,
                routes);
            static_cast<void>(invalidGraph);
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    };

    expectRejectedRole(jamlink::audio::AudioBusRole::NetworkSend);
    expectRejectedRole(jamlink::audio::AudioBusRole::NetworkReceive);
    expectRejectedRole(jamlink::audio::AudioBusRole::RemoteMusic);
    expectRejectedRole(jamlink::audio::AudioBusRole::RemoteVoice);
}

JAMLINK_TEST(gain_stage_ramps_per_frame_and_mutes) {
    jamlink::audio::GainStage gain(1.0F);
    gain.setLinearGain(0.5F);
    std::array<float, 8> stereo{};
    stereo.fill(1.0F);
    gain.process({stereo, 2U});
    EXPECT_TRUE(stereo.front() > stereo.back());
    EXPECT_NEAR(stereo[6], 0.5, 1.0e-6);
    EXPECT_NEAR(stereo[6], stereo[7], 1.0e-6);

    gain.setMuted(true);
    stereo.fill(1.0F);
    gain.process({stereo, 2U});
    EXPECT_NEAR(stereo.back(), 0.0, 1.0e-6);
}

JAMLINK_TEST(level_meter_reports_rms_peak_and_clip_latch) {
    jamlink::audio::LevelMeter meter;
    const std::array samples{0.0F, 0.5F, -0.5F, 1.1F};
    meter.process(samples);
    const auto snapshot = meter.snapshot();
    EXPECT_NEAR(snapshot.peakLinear, 1.1, 1.0e-6);
    EXPECT_TRUE(snapshot.rmsLinear > 0.0F);
    EXPECT_TRUE(snapshot.clipped);
    EXPECT_NEAR(snapshot.peakHoldLinear, 1.1, 1.0e-6);
    EXPECT_TRUE(snapshot.clipSampleCount == 1U);
    EXPECT_TRUE(snapshot.clipEventCount == 1U);

    meter.process(std::array{0.0F, 0.0F});
    EXPECT_TRUE(meter.snapshot().clipped);
    meter.clearClipLatch();
    EXPECT_TRUE(!meter.snapshot().clipped);
}

JAMLINK_TEST(native_input_risk_and_silent_clip_self_test_latch_without_audio_changes) {
    jamlink::audio::LevelMeter nativeInput(
        jamlink::audio::LevelMeter::nativeInputClipThreshold,
        jamlink::audio::LevelMeter::nativeInputRiskThreshold,
        jamlink::audio::LevelMeter::nativeInputRiskSampleCount);

    constexpr float healthyHot = 0.977237F; // -0.2 dBFS
    constexpr float effectivelyFullScale = 0.989F;
    nativeInput.process(std::array{healthyHot, -healthyHot, healthyHot});
    EXPECT_TRUE(!nativeInput.snapshot().clipped);

    nativeInput.process(std::array{effectivelyFullScale, 0.0F});
    nativeInput.process(std::array{-effectivelyFullScale, 0.0F});
    EXPECT_TRUE(!nativeInput.snapshot().clipped);
    nativeInput.process(std::array{effectivelyFullScale, 0.0F});
    const auto risk = nativeInput.snapshot();
    EXPECT_TRUE(risk.clipped);
    EXPECT_TRUE(risk.nearFullScaleRisk);
    EXPECT_TRUE(!risk.diagnosticClip);
    EXPECT_TRUE(risk.clipSampleCount == 0U);
    EXPECT_TRUE(risk.clipEventCount == 0U);

    nativeInput.process(std::array{0.0F, 0.0F});
    EXPECT_TRUE(nativeInput.snapshot().clipped);
    nativeInput.clearClipLatch();
    nativeInput.process(std::array{0.0F, 0.0F});
    EXPECT_TRUE(!nativeInput.snapshot().clipped);
    EXPECT_TRUE(!nativeInput.snapshot().nearFullScaleRisk);

    jamlink::audio::LevelMeter diagnostic;
    constexpr std::array silence{0.0F, 0.0F, 0.0F, 0.0F};
    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_release);
    diagnostic.requestClipSelfTest();
    diagnostic.process(silence);
    allocationTrackingEnabled.store(false, std::memory_order_release);
    const auto tested = diagnostic.snapshot();
    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_relaxed) == 0U);
    EXPECT_TRUE(tested.clipped);
    EXPECT_TRUE(tested.diagnosticClip);
    EXPECT_TRUE(tested.clipSampleCount == 0U);
    EXPECT_TRUE(tested.clipEventCount == 0U);
    diagnostic.clearClipLatch();
    diagnostic.process(silence);
    EXPECT_TRUE(!diagnostic.snapshot().clipped);
    EXPECT_TRUE(!diagnostic.snapshot().diagnosticClip);

    diagnostic.requestClipSelfTest();
    diagnostic.process(std::array{1.0F, 0.0F});
    const auto realClipWins = diagnostic.snapshot();
    EXPECT_TRUE(realClipWins.clipped);
    EXPECT_TRUE(!realClipWins.diagnosticClip);
    EXPECT_TRUE(realClipWins.clipSampleCount == 1U);
}

JAMLINK_TEST(signal_health_distinguishes_hot_clip_internal_and_mix_overload) {
    constexpr float minusSixDbfs = 0.501187F;
    jamlink::audio::LevelMeter guitarInput(
        jamlink::audio::LevelMeter::nativeInputClipThreshold);
    jamlink::audio::LevelMeter voiceInput(
        jamlink::audio::LevelMeter::nativeInputClipThreshold);
    guitarInput.process(std::array{minusSixDbfs, -minusSixDbfs, 0.25F});
    voiceInput.process(std::array{0.8F, -0.8F, 0.0F});
    EXPECT_TRUE(!guitarInput.snapshot().clipped);
    EXPECT_TRUE(!voiceInput.snapshot().clipped);

    guitarInput.process(std::array{0.0F, 1.0F, 0.0F});
    EXPECT_TRUE(guitarInput.snapshot().clipped);
    EXPECT_TRUE(!voiceInput.snapshot().clipped);

    guitarInput.process(std::array{0.0F, 0.0F});
    EXPECT_TRUE(guitarInput.snapshot().clipped);
    guitarInput.clearClipLatch();
    EXPECT_TRUE(!guitarInput.snapshot().clipped);
    guitarInput.process(std::array{1.0F, 1.0F});
    const auto relatched = guitarInput.snapshot();
    EXPECT_TRUE(relatched.clipped);
    EXPECT_TRUE(relatched.clipSampleCount == 2U);
    EXPECT_TRUE(relatched.clipEventCount == 1U);
    EXPECT_NEAR(relatched.peakHoldLinear, 1.0, 1.0e-6);

    // Clean capture can still overload a later gain/send stage.
    const std::array cleanSource{0.7F, -0.7F, 0.25F, -0.25F};
    std::array<float, cleanSource.size()> boosted{};
    std::ranges::transform(cleanSource, boosted.begin(), [](float sample) {
        return sample * 1.6F;
    });
    jamlink::audio::LevelMeter source;
    jamlink::audio::LevelMeter send;
    source.process(cleanSource);
    send.process(boosted);
    EXPECT_TRUE(!source.snapshot().clipped);
    EXPECT_TRUE(send.snapshot().clipped);

    const std::array cleanInstrument{0.65F, -0.65F};
    const std::array cleanVoice{0.55F, -0.55F};
    std::array<float, 2> summed{};
    std::ranges::transform(
        cleanInstrument, cleanVoice, summed.begin(), std::plus<float>{});
    jamlink::audio::LevelMeter instrument;
    jamlink::audio::LevelMeter voice;
    jamlink::audio::LevelMeter monitor;
    instrument.process(cleanInstrument);
    voice.process(cleanVoice);
    monitor.process(summed);
    EXPECT_TRUE(!instrument.snapshot().clipped);
    EXPECT_TRUE(!voice.snapshot().clipped);
    EXPECT_TRUE(monitor.snapshot().clipped);
}

JAMLINK_TEST(level_meter_snapshots_are_coherent_during_concurrent_publication) {
    jamlink::audio::LevelMeter meter;
    constexpr std::array blockA{1.0F, 0.0F};
    constexpr std::array blockB{0.5F, 0.5F};
    std::atomic<bool> producerFinished{false};
    std::atomic<bool> coherent{true};

    std::thread producer([&] {
        for (std::size_t block = 0; block < 100'000U; ++block) {
            meter.process((block & 1U) == 0U ? std::span<const float>(blockA)
                                             : std::span<const float>(blockB));
        }
        producerFinished.store(true, std::memory_order_release);
    });

    while (!producerFinished.load(std::memory_order_acquire)) {
        const auto value = meter.snapshot();
        const bool initial = near(value.peakLinear, 0.0, 1.0e-6)
            && near(value.rmsLinear, 0.0, 1.0e-6);
        const bool fromA = near(value.peakLinear, 1.0, 1.0e-6)
            && near(value.rmsLinear, std::sqrt(0.5), 1.0e-6);
        const bool fromB = near(value.peakLinear, 0.5, 1.0e-6)
            && near(value.rmsLinear, 0.5, 1.0e-6);
        if (!initial && !fromA && !fromB) {
            coherent.store(false, std::memory_order_relaxed);
            break;
        }
    }
    producer.join();
    EXPECT_TRUE(coherent.load(std::memory_order_relaxed));
}

JAMLINK_TEST(processors_contain_nan_and_infinity) {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();

    jamlink::audio::LevelMeter meter;
    meter.process(std::array{nan, infinity, 0.5F, -0.5F});
    const auto meterSnapshot = meter.snapshot();
    EXPECT_TRUE(std::isfinite(meterSnapshot.peakLinear));
    EXPECT_TRUE(std::isfinite(meterSnapshot.rmsLinear));
    EXPECT_TRUE(meterSnapshot.invalidSampleCount == 2U);

    jamlink::audio::GainStage gain;
    std::array gainSamples{nan, infinity, 0.25F, -0.25F};
    gain.process({gainSamples, 1U});
    EXPECT_TRUE(std::ranges::all_of(gainSamples, [](float sample) {
        return std::isfinite(sample);
    }));

    jamlink::audio::PrivateSoundcheckProcessor soundcheck;
    std::array<float, 4> monitor{};
    soundcheck.process({std::array{nan, infinity}, std::array{0.0F, 0.0F}}, monitor);
    EXPECT_TRUE(std::ranges::all_of(monitor, [](float sample) {
        return std::isfinite(sample);
    }));
}

JAMLINK_TEST(private_soundcheck_mixes_only_to_local_stereo_monitor) {
    jamlink::audio::PrivateSoundcheckProcessor soundcheck;
    soundcheck.setMonitorGain(jamlink::audio::SoundcheckSource::Instrument, 1.0F);
    soundcheck.setMonitorGain(jamlink::audio::SoundcheckSource::Voice, 0.5F);
    soundcheck.reset();

    const std::array instrument{1.0F, 1.0F, 1.0F, 1.0F};
    const std::array voice{0.5F, 0.5F, 0.5F, 0.5F};
    std::array<float, 8> monitor{};
    soundcheck.process({instrument, voice}, monitor);
    EXPECT_NEAR(monitor[6], 1.25, 1.0e-6);
    EXPECT_NEAR(monitor[6], monitor[7], 1.0e-6);

    soundcheck.setMonitorEnabled(jamlink::audio::SoundcheckSource::Voice, false);
    soundcheck.process({instrument, voice}, monitor);
    EXPECT_NEAR(monitor[6], 1.0, 1.0e-6);
}

JAMLINK_TEST(realtime_processing_paths_allocate_nothing_after_construction) {
    jamlink::audio::SpscAudioRing ring(256U, 2U);
    jamlink::audio::PrivateSoundcheckProcessor soundcheck;
    jamlink::audio::LevelMeter meter;
    const std::array graphBuses{
        jamlink::audio::AudioBusDefinition{
            31U, jamlink::audio::AudioBusRole::LocalMusic, 1U},
        jamlink::audio::AudioBusDefinition{
            32U, jamlink::audio::AudioBusRole::Monitor, 1U}};
    const std::array graphRoutes{
        jamlink::audio::AudioRouteDefinition{31U, 0U, 32U, 0U, 1.0F}};
    jamlink::audio::AudioRouteGraph graph(
        jamlink::audio::AudioGraphPurpose::PrivateSoundcheck,
        128U,
        graphBuses,
        graphRoutes);
    jamlink::audio::GainStage gain;
    const std::array instrument = [] {
        std::array<float, 128> values{};
        values.fill(0.25F);
        return values;
    }();
    const std::array voice = [] {
        std::array<float, 128> values{};
        values.fill(0.1F);
        return values;
    }();
    std::array<float, 256> monitor{};
    std::array<float, 256> output{};

    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_release);
    meter.process(instrument);
    soundcheck.process({instrument, voice}, monitor);
    static_cast<void>(graph.beginBlock(instrument.size()));
    std::ranges::copy(instrument, graph.mutableBus(31U).begin());
    graph.processRoutes();
    gain.process({monitor, 2U});
    const auto written = ring.write(monitor);
    const auto read = ring.readAndZeroFill(output);
    allocationTrackingEnabled.store(false, std::memory_order_release);

    EXPECT_TRUE(written == 128U);
    EXPECT_TRUE(read == 128U);
    EXPECT_TRUE(std::ranges::equal(graph.bus(32U), instrument));
    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_acquire) == 0U);
}

JAMLINK_TEST(configuration_changes_invalidate_readiness_and_join_safely_muted) {
    using jamlink::control::SetupComponent;
    jamlink::control::ReadinessTracker readiness;
    readiness.setConfiguration(SetupComponent::Instrument, 10U);
    readiness.setConfiguration(SetupComponent::Voice, 20U);
    readiness.setConfiguration(SetupComponent::Output, 30U);
    EXPECT_TRUE(readiness.markVerified(SetupComponent::Instrument, 10U));
    EXPECT_TRUE(readiness.markVerified(SetupComponent::Voice, 20U));
    EXPECT_TRUE(readiness.markVerified(SetupComponent::Output, 30U));
    EXPECT_TRUE(readiness.allVerified());

    readiness.setConfiguration(SetupComponent::Voice, 21U);
    EXPECT_TRUE(!readiness.allVerified());
    const auto safety = readiness.joinSafetyDecision();
    EXPECT_TRUE(!safety.instrumentStartsMuted);
    EXPECT_TRUE(safety.voiceStartsMuted);
    EXPECT_TRUE(safety.outputVerified);
    EXPECT_TRUE(!readiness.markVerified(SetupComponent::Voice, 20U));
}

JAMLINK_TEST(preferences_first_run_and_second_run_restore_stable_ids) {
    const auto directory = std::filesystem::temp_directory_path()
        / "jamlink_preferences_store_tests";
    const auto preferencePath = directory / "preferences.jlpf";
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);

    const jamlink::preferences::PreferencesStore firstRun(preferencePath);
    const auto missing = firstRun.load();
    EXPECT_TRUE(missing.state == jamlink::preferences::PreferencesLoadState::Missing);
    EXPECT_TRUE(missing.preferences.sampleRate == 48'000U);

    auto configured = missing.preferences;
    configured.instrument = {"asio:{interface one}", "input:2", {}};
    configured.voice = {"wasapi:{usb mic}", "capture:0", {}};
    configured.output = {"asio:{interface one}", "output:1", "output:2"};
    configured.sampleRate = 96'000U;
    configured.bufferFrames = 64U;
    configured.instrumentMonitorGain = 0.625F;
    configured.voiceMonitorGain = 0.375F;
    configured.voiceMonitorEnabled = false;
    configured.window = {120, 80, 1'536U, 960U, true};
    EXPECT_TRUE(firstRun.save(configured).succeeded);

    const jamlink::preferences::PreferencesStore secondRun(preferencePath);
    const auto restored = secondRun.load();
    EXPECT_TRUE(restored.state == jamlink::preferences::PreferencesLoadState::Loaded);
    EXPECT_TRUE(restored.preferences == configured);

    std::filesystem::remove_all(directory, cleanupError);
}

JAMLINK_TEST(preferences_recover_defaults_without_overwriting_corrupt_input) {
    const auto directory = std::filesystem::temp_directory_path()
        / "jamlink_preferences_recovery_tests";
    const auto preferencePath = directory / "preferences.jlpf";
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
    std::filesystem::create_directories(directory);
    {
        std::ofstream corrupt(preferencePath, std::ios::binary | std::ios::trunc);
        corrupt << "JAMLINK_PREFERENCES 999\ntruncated";
    }

    const jamlink::preferences::PreferencesStore store(preferencePath);
    const auto recovered = store.load();
    EXPECT_TRUE(
        recovered.state == jamlink::preferences::PreferencesLoadState::RecoveredDefaults);
    EXPECT_TRUE(recovered.preferences == jamlink::preferences::UserPreferences{});
    EXPECT_TRUE(!recovered.diagnostic.empty());
    EXPECT_TRUE(std::filesystem::file_size(preferencePath) > 0U);

    auto invalid = recovered.preferences;
    invalid.window.width = 1U;
    EXPECT_TRUE(!store.save(invalid).succeeded);
    std::filesystem::remove_all(directory, cleanupError);
}

JAMLINK_TEST(simulated_device_hotplug_recovers_without_callback_allocation) {
    class CopyCallback final : public jamlink::audio::IAudioProcessCallback {
    public:
        void process(jamlink::audio::AudioProcessBlock block) noexcept override {
            const std::size_t count = std::min(block.input.samples.size(), block.output.samples.size());
            std::copy_n(block.input.samples.begin(), count, block.output.samples.begin());
            ++calls;
        }

        std::size_t calls{0};
    } callback;

    jamlink::audio::AudioDeviceInfo device;
    device.stableId = "simulated-duplex";
    device.displayName = "Simulated Duplex";
    device.backend = jamlink::audio::AudioBackendKind::Test;
    device.direction = jamlink::audio::AudioDeviceDirection::Duplex;
    device.inputChannels = {{0U, "input-0", "Input 1"}};
    device.outputChannels = {{0U, "output-0", "Output 1"}};
    device.supportedSampleRates = {48'000U};
    device.bufferCapabilities = {4U, 512U, 128U, 0U, {4U, 64U, 128U, 256U, 512U}};
    device.latencyCapabilities = {32U, 48U};
    jamlink::tests::SimulatedAudioDeviceBackend backend({device});
    const jamlink::audio::AudioStreamConfiguration configuration{
        device.stableId, 48'000U, 4U, {0U}, {0U}, 77U};

    const auto firstOpen = backend.open(configuration, callback);
    EXPECT_TRUE(firstOpen.succeeded);
    EXPECT_TRUE(firstOpen.streamInfo.has_value());
    EXPECT_TRUE(firstOpen.streamInfo->inputLatencyFrames == 32U);
    EXPECT_TRUE(firstOpen.streamInfo->outputLatencyFrames == 48U);
    auto invalidConfiguration = configuration;
    invalidConfiguration.inputChannelIndices = {99U};
    EXPECT_TRUE(!backend.open(invalidConfiguration, callback).succeeded);
    EXPECT_TRUE(backend.start());
    const std::array samples{0.25F, 0.5F, -0.5F, -0.25F};
    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_release);
    const bool processed = backend.processOneBlock(samples);
    allocationTrackingEnabled.store(false, std::memory_order_release);
    EXPECT_TRUE(processed);
    EXPECT_TRUE(callback.calls == 1U);
    EXPECT_TRUE(std::ranges::equal(backend.lastOutput(), samples));
    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_acquire) == 0U);

    backend.setPresent(device.stableId, false);
    EXPECT_TRUE(!backend.processOneBlock(samples));
    EXPECT_TRUE(backend.enumerateDevices().empty());
    backend.setPresent(device.stableId, true);
    EXPECT_TRUE(backend.open(configuration, callback).succeeded);
    EXPECT_TRUE(backend.start());
    EXPECT_TRUE(backend.processOneBlock(samples));
    EXPECT_TRUE(callback.calls == 2U);
}

JAMLINK_TEST(simulated_devices_cover_required_selection_topologies) {
    const auto makeDevice = [](
                                std::string id,
                                std::uint32_t inputCount,
                                std::uint32_t outputCount) {
        jamlink::audio::AudioDeviceInfo device;
        device.stableId = std::move(id);
        device.displayName = device.stableId;
        device.backend = jamlink::audio::AudioBackendKind::Test;
        device.direction = jamlink::audio::AudioDeviceDirection::Duplex;
        for (std::uint32_t channel = 0; channel < inputCount; ++channel) {
            device.inputChannels.push_back({
                channel,
                "input-" + std::to_string(channel),
                "Input " + std::to_string(channel + 1U)});
        }
        for (std::uint32_t channel = 0; channel < outputCount; ++channel) {
            device.outputChannels.push_back({
                channel,
                "output-" + std::to_string(channel),
                "Output " + std::to_string(channel + 1U)});
        }
        device.supportedSampleRates = {44'100U, 48'000U, 96'000U};
        device.bufferCapabilities = {64U, 512U, 128U, 64U, {}};
        return device;
    };

    class SilentCallback final : public jamlink::audio::IAudioProcessCallback {
    public:
        void process(jamlink::audio::AudioProcessBlock) noexcept override {}
    } callback;

    const auto interfaceDevice = makeDevice("interface", 4U, 4U);
    const auto usbMicrophone = makeDevice("usb-microphone", 1U, 0U);
    const auto separateOutput = makeDevice("separate-output", 0U, 2U);

    jamlink::tests::SimulatedAudioDeviceBackend oneDeviceBackend({interfaceDevice});
    const jamlink::audio::AudioStreamConfiguration oneDeviceConfiguration{
        interfaceDevice.stableId, 48'000U, 128U, {1U, 0U}, {0U, 1U}, 1U};
    EXPECT_TRUE(oneDeviceBackend.open(oneDeviceConfiguration, callback).succeeded);

    jamlink::tests::SimulatedAudioDeviceBackend instrumentBackend({interfaceDevice});
    jamlink::tests::SimulatedAudioDeviceBackend voiceBackend({usbMicrophone});
    jamlink::tests::SimulatedAudioDeviceBackend outputBackend({interfaceDevice});
    EXPECT_TRUE(instrumentBackend.open(
        {interfaceDevice.stableId, 48'000U, 128U, {1U}, {}, 10U}, callback).succeeded);
    EXPECT_TRUE(voiceBackend.open(
        {usbMicrophone.stableId, 48'000U, 128U, {0U}, {}, 20U}, callback).succeeded);
    EXPECT_TRUE(outputBackend.open(
        {interfaceDevice.stableId, 48'000U, 128U, {}, {0U, 1U}, 10U}, callback).succeeded);

    jamlink::tests::SimulatedAudioDeviceBackend independentOutputBackend({separateOutput});
    EXPECT_TRUE(independentOutputBackend.open(
        {separateOutput.stableId, 48'000U, 128U, {}, {0U, 1U}, 30U}, callback).succeeded);
}

JAMLINK_TEST(clock_controller_is_bounded_and_corrects_fill_direction) {
    jamlink::clock::ClockDomainController controller({
        512U, 1'024U, 0.5, 0.375, 200.0, 0.13});

    for (int iteration = 0; iteration < 1'000; ++iteration) {
        static_cast<void>(controller.update(800U, 128U, 48'000.0));
    }
    EXPECT_TRUE(controller.correctionPpm() > 0.0);
    EXPECT_TRUE(controller.correctionPpm() <= 200.0 + 1.0e-6);

    controller.reset();
    for (int iteration = 0; iteration < 1'000; ++iteration) {
        static_cast<void>(controller.update(200U, 128U, 48'000.0));
    }
    EXPECT_TRUE(controller.correctionPpm() < 0.0);
    EXPECT_TRUE(controller.correctionPpm() >= -200.0 - 1.0e-6);
}

JAMLINK_TEST(async_resampler_is_bounded_allocation_free_and_rate_aware) {
    jamlink::audio::AsyncMonoResampler resampler(4'096U);
    resampler.configure(44'100U, 48'000U);
    std::vector<float> input(441U, 0.0F);
    std::vector<float> output(480U, 0.0F);
    double phase = 0.0;
    constexpr double phaseStep = 6.28318530717958647692 * 997.0 / 44'100.0;

    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_release);
    for (std::size_t block = 0U; block < 2'000U; ++block) {
        for (auto& sample : input) {
            sample = static_cast<float>(std::sin(phase) * 0.5);
            phase += phaseStep;
            if (phase >= 6.28318530717958647692) {
                phase -= 6.28318530717958647692;
            }
        }
        EXPECT_TRUE(resampler.write(input) == input.size());
        static_cast<void>(resampler.read(output));
        EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float sample) {
            return std::isfinite(sample) && std::abs(sample) <= 0.51F;
        }));
    }
    allocationTrackingEnabled.store(false, std::memory_order_release);

    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_relaxed) == 0U);
    EXPECT_TRUE(resampler.availableFrames() < resampler.capacityFrames() / 2U);
    EXPECT_TRUE(resampler.overrunCount() == 0U);
    EXPECT_TRUE(std::abs(resampler.lastCorrectionPpm()) <= 500.0);
}

JAMLINK_TEST(async_resampler_absorbs_virtual_capture_clock_drift) {
    jamlink::audio::AsyncMonoResampler resampler(8'192U);
    resampler.configure(48'000U, 48'000U);
    std::vector<float> input(481U, 0.25F);
    std::vector<float> output(480U, 0.0F);
    double producedRemainder = 0.0;
    std::size_t maximumFill = 0U;

    for (std::size_t block = 0U; block < 36'000U; ++block) {
        producedRemainder += 480.0 * 100.0e-6;
        const std::size_t frames = producedRemainder >= 1.0 ? 481U : 480U;
        if (frames == 481U) {
            producedRemainder -= 1.0;
        }
        EXPECT_TRUE(resampler.write(std::span<const float>(input.data(), frames)) == frames);
        static_cast<void>(resampler.read(output));
        maximumFill = std::max(maximumFill, resampler.availableFrames());
    }

    EXPECT_TRUE(maximumFill < 2'000U);
    EXPECT_TRUE(resampler.overrunCount() == 0U);
    EXPECT_TRUE(std::abs(resampler.lastCorrectionPpm()) <= 500.0);
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float sample) {
        return std::isfinite(sample) && std::abs(sample) <= 0.26F;
    }));
}

JAMLINK_TEST(clock_drift_estimator_tracks_known_rate_offset) {
    jamlink::clock::ClockDriftEstimator estimator(1.0);
    estimator.update(0U, 0U);
    estimator.update(1'000'000U, 1'000'100U);
    EXPECT_TRUE(estimator.hasEstimate());
    EXPECT_NEAR(estimator.driftPpm(), 100.0, 1.0e-6);
}

JAMLINK_TEST(native_sample_conversion_covers_asio_pcm_and_float_formats) {
    constexpr std::array<float, 7U> source{
        -1.0F, -0.5F, -0.125F, 0.0F, 0.125F, 0.5F, 1.0F};
    constexpr std::array formats{
        jamlink::audio::NativeSampleFormat::Int16LittleEndian,
        jamlink::audio::NativeSampleFormat::Int24LittleEndian,
        jamlink::audio::NativeSampleFormat::Int32LittleEndian,
        jamlink::audio::NativeSampleFormat::Float32LittleEndian,
        jamlink::audio::NativeSampleFormat::Float64LittleEndian,
        jamlink::audio::NativeSampleFormat::Int16BigEndian,
        jamlink::audio::NativeSampleFormat::Int24BigEndian,
        jamlink::audio::NativeSampleFormat::Int32BigEndian,
        jamlink::audio::NativeSampleFormat::Float32BigEndian,
        jamlink::audio::NativeSampleFormat::Float64BigEndian};
    std::array<std::uint8_t, 56U> encoded{};
    std::array<float, source.size()> decoded{};

    for (const auto format : formats) {
        std::fill(encoded.begin(), encoded.end(), std::uint8_t{0U});
        std::fill(decoded.begin(), decoded.end(), 0.0F);
        trackedAllocationCount.store(0U, std::memory_order_relaxed);
        allocationTrackingEnabled.store(true, std::memory_order_relaxed);
        jamlink::audio::floatToNativeSamples(format, source, encoded.data());
        jamlink::audio::nativeSamplesToFloat(format, encoded.data(), decoded);
        allocationTrackingEnabled.store(false, std::memory_order_relaxed);
        EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_relaxed) == 0U);
        const double tolerance = jamlink::audio::bytesPerNativeSample(format) == 2U
            ? 4.0e-5 : 2.0e-6;
        for (std::size_t index = 0U; index < source.size(); ++index) {
            EXPECT_NEAR(decoded[index], source[index], tolerance);
        }
    }
}

JAMLINK_TEST(native_float_conversion_sanitizes_non_finite_values) {
    const std::array<float, 4U> source{
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
        2.0F};
    std::array<std::uint8_t, 16U> encoded{};
    std::array<float, 4U> decoded{};
    jamlink::audio::floatToNativeSamples(
        jamlink::audio::NativeSampleFormat::Float32LittleEndian,
        source,
        encoded.data());
    jamlink::audio::nativeSamplesToFloat(
        jamlink::audio::NativeSampleFormat::Float32LittleEndian,
        encoded.data(),
        decoded);
    EXPECT_NEAR(decoded[0], 0.0, 0.0);
    EXPECT_NEAR(decoded[1], 0.0, 0.0);
    EXPECT_NEAR(decoded[2], 0.0, 0.0);
    EXPECT_NEAR(decoded[3], 1.0, 0.0);
}

JAMLINK_TEST(hybrid_clock_bridge_resamples_secondary_voice_without_allocating) {
    jamlink::audio::HybridClockBridge bridge(65'536U, 2'048U, 48'000U);
    bridge.configureStopped(44'100U);
    std::array<float, 441U> input{};
    std::array<float, 480U> output{};
    for (std::size_t index = 0U; index < input.size(); ++index) {
        input[index] = static_cast<float>(index) / static_cast<float>(input.size());
    }
    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_release);
    for (std::size_t block = 0U; block < 20U; ++block) {
        if (block == 10U) {
            const auto generation = bridge.requestSourceTransition(44'100U);
            EXPECT_TRUE(generation != 0U);
            static_cast<void>(bridge.pull(output));
            EXPECT_TRUE(bridge.transitionApplied(generation));
        }
        EXPECT_TRUE(bridge.push(input) == input.size());
        static_cast<void>(bridge.pull(output));
    }
    allocationTrackingEnabled.store(false, std::memory_order_release);
    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_relaxed) == 0U);
    EXPECT_TRUE(std::any_of(output.begin(), output.end(), [](float sample) {
        return sample > 0.01F;
    }));
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float sample) {
        return std::isfinite(sample) && sample >= 0.0F && sample <= 1.0F;
    }));
}

JAMLINK_TEST(hybrid_clock_bridge_absorbs_drift_at_48k_and_44k1) {
    constexpr std::size_t outputFrames = 128U;
    for (const std::uint32_t sourceRate : std::array{44'100U, 48'000U}) {
        for (const double driftPpm : std::array{-120.0, 120.0}) {
            jamlink::audio::HybridClockBridge bridge(65'536U, 2'048U, 48'000U);
            bridge.configureStopped(sourceRate);
            std::array<float, 256U> input{};
            std::array<float, outputFrames> output{};
            std::fill(input.begin(), input.end(), 0.2F);
            double sourceAccumulator = 0.0;
            std::size_t maximumOccupancy = 0U;
            constexpr std::size_t oneHourBlocks = 1'350'000U;
            for (std::size_t block = 0U; block < oneHourBlocks; ++block) {
                sourceAccumulator += static_cast<double>(outputFrames)
                    * static_cast<double>(sourceRate) / 48'000.0
                    * (1.0 + driftPpm * 1.0e-6);
                const auto frames = static_cast<std::size_t>(std::floor(sourceAccumulator));
                sourceAccumulator -= static_cast<double>(frames);
                EXPECT_TRUE(frames <= input.size());
                static_cast<void>(bridge.push(
                    std::span<const float>(input.data(), frames)));
                static_cast<void>(bridge.pull(output));
                maximumOccupancy = std::max(
                    maximumOccupancy, bridge.sourceOccupancyFrames());
                EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float sample) {
                    return std::isfinite(sample) && std::abs(sample) <= 0.21F;
                }));
            }
            EXPECT_TRUE(maximumOccupancy < 2'048U);
            EXPECT_TRUE(bridge.overrunCount() == 0U);
            EXPECT_TRUE(std::abs(bridge.correctionPpm()) <= 500.0);
        }
    }
}

JAMLINK_TEST(hybrid_clock_bridge_recovers_from_disconnect_and_source_replacement) {
    jamlink::audio::HybridClockBridge bridge(65'536U, 2'048U, 48'000U);
    bridge.configureStopped(48'000U);
    std::array<float, 128U> input{};
    std::array<float, 128U> output{};
    std::fill(input.begin(), input.end(), 0.25F);
    for (std::size_t block = 0U; block < 8U; ++block) {
        static_cast<void>(bridge.push(input));
        static_cast<void>(bridge.pull(output));
    }
    for (std::size_t block = 0U; block < 1'000U; ++block) {
        static_cast<void>(bridge.pull(output));
    }
    EXPECT_TRUE(std::all_of(output.begin(), output.end(), [](float sample) {
        return sample == 0.0F;
    }));

    const std::uint32_t transition = bridge.requestSourceTransition(44'100U);
    static_cast<void>(bridge.pull(output));
    EXPECT_TRUE(bridge.transitionApplied(transition));
    std::array<float, 118U> replacement{};
    std::fill(replacement.begin(), replacement.end(), -0.2F);
    for (std::size_t block = 0U; block < 12U; ++block) {
        static_cast<void>(bridge.push(replacement));
        static_cast<void>(bridge.pull(output));
    }
    EXPECT_TRUE(std::any_of(output.begin(), output.end(), [](float sample) {
        return sample < -0.01F;
    }));
    EXPECT_TRUE(bridge.overrunCount() == 0U);
}

JAMLINK_TEST(hybrid_clock_bridge_is_lock_free_under_concurrent_capture_and_render) {
    jamlink::audio::HybridClockBridge bridge(65'536U, 2'048U, 48'000U);
    bridge.configureStopped(48'000U);
    constexpr std::size_t blockFrames = 128U;
    constexpr std::size_t blockCount = 20'000U;
    std::atomic<std::size_t> publishedBlocks{0U};
    std::atomic<std::size_t> consumedBlocks{0U};
    std::atomic<bool> producerFinished{false};
    std::atomic<bool> validOutput{true};
    std::atomic<std::size_t> nonSilentBlocks{0U};

    std::thread producer([&] {
        std::array<float, blockFrames> input{};
        for (std::size_t block = 0U; block < blockCount; ++block) {
            while (block > consumedBlocks.load(std::memory_order_acquire) + 16U) {
                std::this_thread::yield();
            }
            const float value = (block & 1U) == 0U ? 0.25F : -0.25F;
            std::fill(input.begin(), input.end(), value);
            while (bridge.push(input) != input.size()) {
                std::this_thread::yield();
            }
            publishedBlocks.store(block + 1U, std::memory_order_release);
        }
        producerFinished.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        std::array<float, blockFrames> output{};
        while (!producerFinished.load(std::memory_order_acquire)
               || consumedBlocks.load(std::memory_order_acquire)
                    < publishedBlocks.load(std::memory_order_acquire)) {
            static_cast<void>(bridge.pull(output));
            if (!std::all_of(output.begin(), output.end(), [](float sample) {
                    return std::isfinite(sample) && std::abs(sample) <= 0.251F;
                })) {
                validOutput.store(false, std::memory_order_relaxed);
            }
            if (std::any_of(output.begin(), output.end(), [](float sample) {
                    return std::abs(sample) > 0.01F;
                })) {
                nonSilentBlocks.fetch_add(1U, std::memory_order_relaxed);
            }
            const auto consumed = consumedBlocks.load(std::memory_order_relaxed);
            consumedBlocks.store(
                std::min(publishedBlocks.load(std::memory_order_acquire), consumed + 1U),
                std::memory_order_release);
        }
    });
    producer.join();
    consumer.join();

    EXPECT_TRUE(validOutput.load(std::memory_order_relaxed));
    EXPECT_TRUE(nonSilentBlocks.load(std::memory_order_relaxed) > blockCount / 2U);
    EXPECT_TRUE(bridge.overrunCount() == 0U);
}

JAMLINK_TEST(clock_controller_bounds_occupancy_for_virtual_eight_hour_drift) {
    constexpr std::size_t blocksPerEightHours = 10'800'000U;
    for (const double driftPpm : std::array{-100.0, 100.0}) {
        jamlink::clock::ClockDomainController controller({
            512U, 1'024U, 0.35, 0.1875, 250.0, 0.13});
        double fillFrames = 512.0;
        double minimumFill = fillFrames;
        double maximumFill = fillFrames;

        for (std::size_t block = 0; block < blocksPerEightHours; ++block) {
            const double producerFrames = 128.0 * (1.0 + driftPpm * 1.0e-6);
            const double consumerFrames = 128.0 * controller.correctionRatio();
            fillFrames += producerFrames - consumerFrames;
            minimumFill = std::min(minimumFill, fillFrames);
            maximumFill = std::max(maximumFill, fillFrames);
            static_cast<void>(controller.update(
                static_cast<std::size_t>(std::round(fillFrames)), 128U, 48'000.0));
        }

        EXPECT_TRUE(std::isfinite(fillFrames));
        EXPECT_TRUE(minimumFill > 400.0);
        EXPECT_TRUE(maximumFill < 625.0);
        EXPECT_NEAR(controller.correctionPpm(), driftPpm, 0.01);
    }
}

JAMLINK_TEST(clock_controller_is_stable_across_callback_cadence) {
    struct SimulationResult final {
        double correctionPpm{0.0};
        double minimumFill{0.0};
        double maximumFill{0.0};
    };

    const auto simulate = [](std::size_t blockFrames, double sampleRate) {
        jamlink::clock::ClockDomainController controller({
            512U, 1'024U, 0.35, 0.1875, 250.0, 0.13});
        double fillFrames = 512.0;
        double minimumFill = fillFrames;
        double maximumFill = fillFrames;
        const auto blockCount = static_cast<std::size_t>(
            sampleRate / static_cast<double>(blockFrames) * 3'600.0);

        for (std::size_t block = 0; block < blockCount; ++block) {
            fillFrames += static_cast<double>(blockFrames) * (1.0 + 100.0e-6)
                - static_cast<double>(blockFrames) * controller.correctionRatio();
            minimumFill = std::min(minimumFill, fillFrames);
            maximumFill = std::max(maximumFill, fillFrames);
            static_cast<void>(controller.update(
                static_cast<std::size_t>(std::round(fillFrames)),
                blockFrames,
                sampleRate));
        }
        return SimulationResult{controller.correctionPpm(), minimumFill, maximumFill};
    };

    const auto fastSmall = simulate(64U, 96'000.0);
    const auto baseline = simulate(128U, 48'000.0);
    const auto slowLarge = simulate(512U, 44'100.0);
    for (const auto result : std::array{fastSmall, baseline, slowLarge}) {
        EXPECT_NEAR(result.correctionPpm, 100.0, 0.05);
        EXPECT_TRUE(result.minimumFill > 400.0);
        EXPECT_TRUE(result.maximumFill < 625.0);
    }
    EXPECT_NEAR(fastSmall.correctionPpm, slowLarge.correctionPpm, 0.01);
}

} // namespace

int main() {
    std::size_t failures = 0;
    for (const auto& test : tests()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }

    std::cout << (tests().size() - failures) << '/' << tests().size() << " tests passed\n";
    return failures == 0U ? 0 : 1;
}
