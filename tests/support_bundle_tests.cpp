// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// A support bundle is pasted into chat windows and attached to public issues by
// people who are asking for help, not auditing what they are sending. A privacy
// failure here is a serious defect, so these tests deliberately seed
// secret-looking values everywhere free text can enter and require that none of
// them survive.

#include "jamlink/diagnostics/support_bundle.hpp"

#include <cstddef>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
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

using jamlink::diagnostics::SupportSnapshot;
using jamlink::diagnostics::renderSupportBundle;
using jamlink::diagnostics::renderSupportBundleJson;
using jamlink::diagnostics::sanitiseForSupport;

// Shaped exactly like the things that must never escape.
constexpr const char* roomSecret = "a3f5c8d9e1b24760af38c2d5e6b7190c";
constexpr const char* fullInvite =
    "JL1|203.0.113.7|41234|a3f5c8d9e1b24760af38c2d5e6b7190c";

[[nodiscard]] SupportSnapshot realistic() {
    SupportSnapshot snapshot;
    snapshot.applicationVersion = "0.4.1-test";
    snapshot.buildIdentity = "51df95f0000000000000000000000000000000ab";
    snapshot.releaseChannel = "test";
    snapshot.mediaProtocolVersion = 3U;
    snapshot.controlProtocolVersion = 2U;
    snapshot.audioCodec = "opus";
    snapshot.codecBitsPerSecond = 96'000U;
    snapshot.codecFrameMilliseconds = 5U;
    snapshot.opusPacketsDecoded = 12'400U;
    snapshot.instrumentDevice = "Focusrite USB ASIO - Input 1";
    snapshot.instrumentBackend = "ASIO";
    snapshot.voiceDevice = "Yeti Stereo Microphone";
    snapshot.voiceBackend = "Windows shared";
    snapshot.outputDevice = "Focusrite USB ASIO - Outputs 1-2";
    snapshot.outputBackend = "ASIO";
    snapshot.audioTopologySupported = true;
    snapshot.audioTopologyReason = "ValidAsioWithSecondaryWasapiVoice";
    snapshot.audioRunning = true;
    snapshot.sampleRate = 48'000U;
    snapshot.runningBufferFrames = 128U;
    snapshot.roundTripMeasured = true;
    snapshot.roundTripMilliseconds = 14U;
    snapshot.lifecycleTransitions = {"Connecting -> ReadyToPlay", "ReadyToPlay -> Degraded"};
    snapshot.logExcerpt = {"mapping upnp granted", "handshake complete"};
    return snapshot;
}

// A two-home field test was diagnosed from a bundle that said "backend: WASAPI
// shared" while the guitar was on an interface driver, "codec: pcm16" when
// nothing had been decoded at all, and "sample rate: 0" for an engine that
// never opened. Every one of those read as a measurement.
JAMLINK_TEST(a_mixed_audio_setup_is_not_reported_as_a_uniform_one) {
    SupportSnapshot snapshot = realistic();
    snapshot.instrumentBackend = "ASIO";
    snapshot.voiceBackend = "Windows shared";
    snapshot.outputBackend = "Windows shared";
    snapshot.audioTopologySupported = false;
    snapshot.audioTopologyReason = "AsioInstrumentRequiresAsioOutput";

    const std::string text = renderSupportBundle(snapshot);
    // All three endpoints are named, so no single line can stand for a setup
    // whose parts disagree.
    EXPECT_TRUE(text.find("instrument audio system: ASIO") != std::string::npos);
    EXPECT_TRUE(text.find("voice audio system: Windows shared") != std::string::npos);
    EXPECT_TRUE(text.find("output audio system: Windows shared") != std::string::npos);
    // And the verdict is stated rather than left to be re-derived by whoever
    // reads the bundle.
    EXPECT_TRUE(text.find("can run together: no") != std::string::npos);
    EXPECT_TRUE(text.find("AsioInstrumentRequiresAsioOutput") != std::string::npos);
}

JAMLINK_TEST(an_engine_that_never_started_says_so_instead_of_reporting_zero) {
    SupportSnapshot snapshot = realistic();
    snapshot.audioRunning = false;
    snapshot.sampleRate = 0U;
    snapshot.runningBufferFrames = 0U;

    const std::string text = renderSupportBundle(snapshot);
    EXPECT_TRUE(text.find("engine running: no") != std::string::npos);
    // "sample rate: 0" invites a reader to look for a device running at no
    // rate. There was no device.
    EXPECT_TRUE(text.find("sample rate: 0") == std::string::npos);
    EXPECT_TRUE(text.find("running buffer: 0") == std::string::npos);
    EXPECT_TRUE(text.find("sample rate: (engine never started)") != std::string::npos);
    EXPECT_TRUE(text.find("running buffer: (engine never started)") != std::string::npos);

    // A running engine must still report its real numbers, including a
    // genuinely zero one.
    snapshot.audioRunning = true;
    const std::string running = renderSupportBundle(snapshot);
    EXPECT_TRUE(running.find("sample rate: 0") != std::string::npos);
    EXPECT_TRUE(running.find("(engine never started)") == std::string::npos);
}

JAMLINK_TEST(a_room_secret_never_reaches_the_bundle) {
    SupportSnapshot snapshot = realistic();
    // Seeded into every surface free text can enter.
    snapshot.logExcerpt = {
        std::string("joining room with key ") + roomSecret,
        std::string("invite ") + fullInvite,
    };
    snapshot.lifecycleTransitions = {
        std::string("Idle -> Connecting (") + roomSecret + ")"};

    const std::string text = renderSupportBundle(snapshot);
    const std::string json = renderSupportBundleJson(snapshot);

    EXPECT_TRUE(text.find(roomSecret) == std::string::npos);
    EXPECT_TRUE(json.find(roomSecret) == std::string::npos);
    // The address is worth keeping for diagnosis; the secret after it is not.
    EXPECT_TRUE(text.find("203.0.113.7") != std::string::npos);
    EXPECT_TRUE(text.find("<secret withheld>") != std::string::npos);
}

JAMLINK_TEST(the_redaction_survives_a_secret_split_across_fields) {
    // A key does not have to arrive alone on a line to need removing.
    SupportSnapshot snapshot = realistic();
    snapshot.logExcerpt = {
        std::string("prefix ") + roomSecret + " suffix",
        std::string(roomSecret) + "trailing",
        std::string("leading") + roomSecret,
    };
    const std::string text = renderSupportBundle(snapshot);
    EXPECT_TRUE(text.find(roomSecret) == std::string::npos);
    // The surrounding words are kept, so the line still means something.
    EXPECT_TRUE(text.find("prefix") != std::string::npos);
    EXPECT_TRUE(text.find("suffix") != std::string::npos);
}

JAMLINK_TEST(the_bundle_carries_no_field_a_secret_could_hide_in) {
    // The strongest guarantee available: the bundle is an allowlist of typed
    // facts, so anything not declared has nowhere to appear. A default snapshot
    // must therefore render without any caller-supplied text at all.
    const SupportSnapshot empty;
    const std::string text = renderSupportBundle(empty);
    EXPECT_TRUE(text.find(roomSecret) == std::string::npos);
    EXPECT_TRUE(text.find("JL1|") == std::string::npos);
    // And it still says what it is, so an empty bundle is not a confusing one.
    EXPECT_TRUE(text.find("JamLink support bundle") != std::string::npos);
    EXPECT_TRUE(text.find("no room secrets") != std::string::npos);
}

JAMLINK_TEST(both_renderings_agree_because_they_share_one_snapshot) {
    // A preview that disagreed with the attached file would send a musician and
    // whoever is helping them chasing different facts.
    const SupportSnapshot snapshot = realistic();
    const std::string text = renderSupportBundle(snapshot);
    const std::string json = renderSupportBundleJson(snapshot);

    EXPECT_TRUE(text.find("0.4.1-test") != std::string::npos);
    EXPECT_TRUE(json.find("0.4.1-test") != std::string::npos);
    EXPECT_TRUE(text.find("opus") != std::string::npos);
    EXPECT_TRUE(json.find("\"audioCodec\":\"opus\"") != std::string::npos);
    EXPECT_TRUE(text.find("96000") != std::string::npos);
    EXPECT_TRUE(json.find("\"codecBitsPerSecond\":96000") != std::string::npos);
    EXPECT_TRUE(json.find("\"roundTripMilliseconds\":14") != std::string::npos);
}

JAMLINK_TEST(the_facts_worth_having_are_actually_there) {
    // The point of the artefact: a bad-audio report should be answerable from
    // it without another round trip.
    const std::string text = renderSupportBundle(realistic());
    EXPECT_TRUE(text.find("ASIO") != std::string::npos);
    EXPECT_TRUE(text.find("Focusrite") != std::string::npos);
    EXPECT_TRUE(text.find("48000") != std::string::npos);
    EXPECT_TRUE(text.find("128") != std::string::npos);
    EXPECT_TRUE(text.find("Connecting -> ReadyToPlay") != std::string::npos);
    EXPECT_TRUE(text.find("mapping upnp granted") != std::string::npos);
}

JAMLINK_TEST(json_stays_parseable_when_text_contains_quotes_and_newlines) {
    SupportSnapshot snapshot = realistic();
    snapshot.logExcerpt = {"device \"Focusrite\" opened\nsecond line\ttabbed"};
    const std::string json = renderSupportBundleJson(snapshot);
    // The quote is escaped rather than closing the string early.
    EXPECT_TRUE(json.find("\\\"Focusrite\\\"") != std::string::npos);
    // A raw newline inside a JSON string is invalid; it must not survive as one.
    const auto logAt = json.find("\"logExcerpt\"");
    EXPECT_TRUE(logAt != std::string::npos);
    EXPECT_TRUE(json.find('\n', logAt) == std::string::npos);
}

JAMLINK_TEST(sanitising_is_idempotent) {
    // A line that has already been through the log's redaction must not be
    // mangled further, or a bundle would disagree with the log it came from.
    const std::string once = sanitiseForSupport(
        std::string("key ") + roomSecret + " end");
    const std::string twice = sanitiseForSupport(once);
    EXPECT_TRUE(once == twice);
    EXPECT_TRUE(once.find("<redacted>") != std::string::npos);
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
              << " support bundle tests passed\n";
    return failures == 0U ? 0 : 1;
}
