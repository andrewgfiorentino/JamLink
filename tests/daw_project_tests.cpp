// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// A take is only worth what a musician can do with it. These check that the
// alignment JamLink knows for certain survives into something that opens.

#include "jamlink/record/daw_project.hpp"

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

using jamlink::record::TakeManifest;
using jamlink::record::TakeSource;
using jamlink::record::writeReaperProject;

[[nodiscard]] TakeSource source(
    const char* role, const char* origin, const char* who, const char* file) {
    TakeSource result;
    result.sourceId = std::string(origin) + ":" + role;
    result.participantId = who;
    result.role = role;
    result.origin = origin;
    result.fileName = file;
    return result;
}

[[nodiscard]] TakeManifest realistic() {
    TakeManifest manifest;
    manifest.takeId = "take-1";
    manifest.sampleRate = 48'000U;
    manifest.startedAtMillisecond = 1'000U;
    manifest.endedAtMillisecond = 91'000U;  // ninety seconds
    manifest.sources = {
        source("instrument", "local-capture", "Andrew", "instrument.wav"),
        source("voice", "local-capture", "Andrew", "voice.wav"),
        source("instrument", "local-original", "Andrew", "instrument-original.wav"),
        source("instrument", "network-received", "Mike", "friend-instrument.wav"),
    };
    return manifest;
}

[[nodiscard]] std::size_t occurrences(const std::string& text, const std::string& needle) {
    std::size_t count = 0U;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++count;
    }
    return count;
}

JAMLINK_TEST(every_source_becomes_a_track_starting_at_the_same_instant) {
    // The alignment is the fact worth writing down. Any track that did not
    // start at zero would silently ruin the take it was meant to make usable.
    const std::string project = writeReaperProject(realistic());
    EXPECT_TRUE(occurrences(project, "<TRACK") == 4U);
    EXPECT_TRUE(occurrences(project, "POSITION 0") == 4U);
    EXPECT_TRUE(project.find("instrument.wav") != std::string::npos);
    EXPECT_TRUE(project.find("friend-instrument.wav") != std::string::npos);
}

JAMLINK_TEST(a_track_says_whose_it_is_and_whether_it_was_played_or_received) {
    // "instrument.wav" and "friend-instrument.wav" are indistinguishable once
    // they are four unnamed lanes in a project window.
    const std::string project = writeReaperProject(realistic());
    EXPECT_TRUE(project.find("Andrew - guitar (heard)") != std::string::npos);
    EXPECT_TRUE(project.find("Andrew - voice (heard)") != std::string::npos);
    // The distinction that matters most while mixing: one of these survived
    // the network and the other never touched it.
    EXPECT_TRUE(project.find("Andrew - guitar (played)") != std::string::npos);
    EXPECT_TRUE(project.find("Mike - guitar (received)") != std::string::npos);
}

JAMLINK_TEST(a_source_left_out_of_the_take_is_left_out_of_the_project) {
    // Excluded sources are not in the manifest, so a project built from it
    // cannot reference a file that was deliberately never written.
    TakeManifest manifest = realistic();
    manifest.sources.erase(manifest.sources.begin() + 1);
    manifest.excludedSources = {"local-capture:voice"};
    const std::string project = writeReaperProject(manifest);
    EXPECT_TRUE(occurrences(project, "<TRACK") == 3U);
    EXPECT_TRUE(project.find("\"voice.wav\"") == std::string::npos);
}

JAMLINK_TEST(the_length_comes_from_the_take_not_from_one_file) {
    // The local originals run at their own capture rate, so a frame count
    // taken from any one file would misdescribe the others.
    const std::string project = writeReaperProject(realistic());
    EXPECT_TRUE(occurrences(project, "LENGTH 90") == 4U);
}

JAMLINK_TEST(a_quote_in_a_name_cannot_break_the_file) {
    // Display names are typed by the musician, so this is reachable rather
    // than theoretical, and the failure is a project that will not open at all.
    TakeManifest manifest = realistic();
    manifest.sources[0].participantId = "And\"rew";
    const std::string project = writeReaperProject(manifest);
    EXPECT_TRUE(project.find("And\"rew") == std::string::npos);
    // Every quote still pairs up, which is what "will open" reduces to here.
    EXPECT_TRUE(occurrences(project, "\"") % 2U == 0U);
}

JAMLINK_TEST(a_nameless_participant_still_gets_a_readable_track) {
    // A take recorded before a profile was filled in must not produce four
    // lanes a musician has to solo one at a time to identify.
    TakeManifest manifest = realistic();
    for (auto& item : manifest.sources) {
        item.participantId.clear();
    }
    const std::string project = writeReaperProject(manifest);
    EXPECT_TRUE(project.find("You - guitar (heard)") != std::string::npos);
    EXPECT_TRUE(project.find("Your friend - guitar (received)") != std::string::npos);
    EXPECT_TRUE(project.find("NAME \" -") == std::string::npos);
}

JAMLINK_TEST(an_empty_take_produces_a_valid_empty_project) {
    TakeManifest manifest;
    const std::string project = writeReaperProject(manifest);
    EXPECT_TRUE(project.rfind("<REAPER_PROJECT", 0U) == 0U);
    EXPECT_TRUE(occurrences(project, "<TRACK") == 0U);
    // Still a well-formed file rather than a fragment.
    EXPECT_TRUE(project.back() == '\n');
    EXPECT_TRUE(project.find(">\n") != std::string::npos);
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
              << " DAW project tests passed\n";
    return failures == 0U ? 0 : 1;
}
