// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// The configurations a musician can actually build out of the device pickers,
// and which of them can run.
//
// Case C below is a real two-home field test that failed. Every device chosen
// was real and two of them were the same physical interface; the graph was
// still impossible, and JamLink reported "unsupported Windows format" while the
// audio engine never started at all.

#include "jamlink/audio/audio_topology.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

#define JAMLINK_TEST(name) \
    void name(); \
    const RegisterTest register_##name(#name, name); \
    void name()

#define EXPECT_TRUE(expression) \
    do { if (!(expression)) { fail(#expression, __FILE__, __LINE__); } } while (false)

using jamlink::audio::AudioTopology;
using jamlink::audio::AudioTopologyEndpoint;
using jamlink::audio::AudioTopologyReason;
using jamlink::audio::SoundcheckBackend;
using jamlink::audio::evaluateAudioTopology;

// The real hardware from the field test.
constexpr const char* focusrite = "asio:Focusrite USB ASIO";
constexpr const char* otherAsio = "asio:ASIO4ALL v2";

[[nodiscard]] AudioTopologyEndpoint asioEndpoint(const char* driver, const char* label) {
    return AudioTopologyEndpoint{SoundcheckBackend::Asio, driver, label};
}

[[nodiscard]] AudioTopologyEndpoint windowsEndpoint(const char* id, const char* label) {
    return AudioTopologyEndpoint{SoundcheckBackend::WasapiShared, id, label};
}

JAMLINK_TEST(case_a_everything_through_windows_audio_works) {
    // A machine with no interface at all. Must keep working.
    AudioTopology topology;
    topology.instrument = windowsEndpoint("wasapi:line-in", "Line In");
    topology.voice = windowsEndpoint("wasapi:yeti", "Yeti Stereo Microphone");
    topology.output = windowsEndpoint("wasapi:speakers", "Speakers");
    const auto result = evaluateAudioTopology(topology);
    EXPECT_TRUE(result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::ValidWasapiGraph);
    EXPECT_TRUE(result.masterBackend == SoundcheckBackend::WasapiShared);
    EXPECT_TRUE(!result.hybridVoice);
}

JAMLINK_TEST(case_b_one_interface_for_everything_works) {
    AudioTopology topology;
    topology.instrument = asioEndpoint(focusrite, "Focusrite USB ASIO - Input 1");
    topology.voice = asioEndpoint(focusrite, "Focusrite USB ASIO - Input 2");
    topology.output = asioEndpoint(focusrite, "Focusrite USB ASIO - Outputs 1-2");
    const auto result = evaluateAudioTopology(topology);
    EXPECT_TRUE(result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::ValidAsioGraph);
    EXPECT_TRUE(result.masterBackend == SoundcheckBackend::Asio);
    EXPECT_TRUE(!result.hybridVoice);
}

JAMLINK_TEST(case_c_interface_for_guitar_with_a_usb_microphone_works) {
    // The configuration JamLink is meant to support, and the one a musician
    // most plausibly owns: a proper interface for guitar and headphones, and a
    // USB microphone that will never speak ASIO.
    AudioTopology topology;
    topology.instrument = asioEndpoint(focusrite, "Focusrite USB ASIO - Input 1");
    topology.voice = windowsEndpoint("wasapi:yeti", "Yeti Stereo Microphone");
    topology.output = asioEndpoint(focusrite, "Focusrite USB ASIO - Outputs 1-2");
    const auto result = evaluateAudioTopology(topology);
    EXPECT_TRUE(result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::ValidAsioWithSecondaryWasapiVoice);
    // The interface keeps the clock; the microphone is bridged into it.
    EXPECT_TRUE(result.masterBackend == SoundcheckBackend::Asio);
    EXPECT_TRUE(result.hybridVoice);
}

JAMLINK_TEST(case_d_the_field_test_failure_is_diagnosed_not_merely_refused) {
    // Exactly what was selected in the failing session: the interface's own
    // input, a Yeti, and the same interface's Windows output.
    AudioTopology topology;
    topology.instrument = asioEndpoint(focusrite, "Focusrite USB ASIO - Input 1");
    topology.voice = windowsEndpoint("wasapi:yeti", "Yeti Stereo Microphone");
    topology.output = windowsEndpoint("wasapi:focusrite", "Speakers (Focusrite USB Audio)");
    const auto result = evaluateAudioTopology(topology);

    EXPECT_TRUE(!result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::AsioInstrumentRequiresAsioOutput);
    // One thing to change, and it is the output rather than the microphone.
    EXPECT_TRUE(result.changeOutput);
    EXPECT_TRUE(!result.changeVoice);
    EXPECT_TRUE(!result.changeInstrument);
    // The interface to change it to is named, so the fix can be one click.
    EXPECT_TRUE(result.requiredEndpointId == focusrite);

    // The advice must be about devices, not about audio APIs. "Unsupported
    // Windows format" was true of nothing here and told the musician nothing.
    const std::string_view advice = result.advice();
    EXPECT_TRUE(!advice.empty());
    EXPECT_TRUE(advice.find("ASIO") == std::string_view::npos);
    EXPECT_TRUE(advice.find("WASAPI") == std::string_view::npos);
    EXPECT_TRUE(advice.find("format") == std::string_view::npos);
    // And it should say the microphone is not the problem, because that is the
    // device a musician would otherwise start changing first.
    EXPECT_TRUE(advice.find("microphone") != std::string_view::npos);
}

JAMLINK_TEST(the_mirror_image_is_diagnosed_too) {
    AudioTopology topology;
    topology.instrument = windowsEndpoint("wasapi:line-in", "Line In");
    topology.voice = windowsEndpoint("wasapi:yeti", "Yeti");
    topology.output = asioEndpoint(focusrite, "Focusrite USB ASIO - Outputs 1-2");
    const auto result = evaluateAudioTopology(topology);
    EXPECT_TRUE(!result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::AsioOutputRequiresAsioInstrument);
    EXPECT_TRUE(result.changeInstrument);
    EXPECT_TRUE(result.requiredEndpointId == focusrite);
}

JAMLINK_TEST(two_different_interfaces_cannot_both_hold_the_clock) {
    AudioTopology topology;
    topology.instrument = asioEndpoint(focusrite, "Focusrite - Input 1");
    topology.voice = windowsEndpoint("wasapi:yeti", "Yeti");
    topology.output = asioEndpoint(otherAsio, "ASIO4ALL - Output");
    const auto result = evaluateAudioTopology(topology);
    EXPECT_TRUE(!result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::AsioDriverMismatch);
    EXPECT_TRUE(result.requiredEndpointId == focusrite);
}

JAMLINK_TEST(an_asio_microphone_on_a_second_interface_is_refused_with_a_way_out) {
    // Its plain Windows entry would work through the bridge, so the advice
    // offers that rather than only saying no.
    AudioTopology topology;
    topology.instrument = asioEndpoint(focusrite, "Focusrite - Input 1");
    topology.voice = asioEndpoint(otherAsio, "ASIO4ALL - Input 1");
    topology.output = asioEndpoint(focusrite, "Focusrite - Outputs 1-2");
    const auto result = evaluateAudioTopology(topology);
    EXPECT_TRUE(!result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::AsioVoiceDriverMismatch);
    EXPECT_TRUE(result.changeVoice);
    EXPECT_TRUE(result.advice().find("Windows") != std::string_view::npos);
}

JAMLINK_TEST(a_missing_device_is_reported_as_missing) {
    AudioTopology topology;
    topology.instrument = asioEndpoint(focusrite, "Focusrite - Input 1");
    topology.output = asioEndpoint(focusrite, "Focusrite - Outputs 1-2");
    const auto result = evaluateAudioTopology(topology);
    EXPECT_TRUE(!result.supported);
    EXPECT_TRUE(result.reason == AudioTopologyReason::MissingEndpoint);
}

JAMLINK_TEST(a_working_configuration_never_offers_advice) {
    // Advice on a healthy setup is noise, and the interface decides whether to
    // show anything from whether this is empty.
    const std::vector<AudioTopology> healthy{
        AudioTopology{windowsEndpoint("a", "A"), windowsEndpoint("b", "B"),
                      windowsEndpoint("c", "C")},
        AudioTopology{asioEndpoint(focusrite, "I"), windowsEndpoint("y", "Y"),
                      asioEndpoint(focusrite, "O")},
        AudioTopology{asioEndpoint(focusrite, "I"), asioEndpoint(focusrite, "V"),
                      asioEndpoint(focusrite, "O")}};
    for (const auto& topology : healthy) {
        const auto result = evaluateAudioTopology(topology);
        EXPECT_TRUE(result.supported);
        EXPECT_TRUE(result.advice().empty());
    }
}

JAMLINK_TEST(every_reason_is_named_for_diagnostics) {
    for (std::uint8_t raw = 0U;
         raw <= static_cast<std::uint8_t>(AudioTopologyReason::MissingEndpoint); ++raw) {
        const auto reason = static_cast<AudioTopologyReason>(raw);
        EXPECT_TRUE(!jamlink::audio::topologyReasonName(reason).empty());
        EXPECT_TRUE(jamlink::audio::topologyReasonName(reason) != "Unknown");
    }
}

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& test : tests()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests().size() - failures) << '/' << tests().size()
              << " audio topology tests passed\n";
    return failures == 0U ? 0 : 1;
}
