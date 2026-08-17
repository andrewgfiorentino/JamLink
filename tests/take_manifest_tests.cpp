// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// What a take owes a musician.
//
// The rule everything here serves: a recording is never presented as clean
// unless it finished. A playable WAV proves samples reached a disk, not that
// the take completed, and an interrupted recording opens perfectly while
// missing its last ten seconds. These tests exist so that difference can never
// be silently lost.

#include "jamlink/record/take_manifest.hpp"

#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
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

using jamlink::record::TakeJournal;
using jamlink::record::TakeManifest;
using jamlink::record::TakeSource;
using jamlink::record::TakeState;
using jamlink::record::parseTake;
using jamlink::record::serialiseTake;

[[nodiscard]] std::filesystem::path scratchRoot() {
    static int counter = 0;
    const auto root = std::filesystem::temp_directory_path()
        / ("jamlink-take-tests-" + std::to_string(++counter));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    std::filesystem::create_directories(root, error);
    return root;
}

[[nodiscard]] TakeManifest sampleTake() {
    TakeManifest manifest;
    manifest.takeId = "take-2026-08-17-2140";
    manifest.sessionId = "session-91";
    manifest.startedAtMillisecond = 1'700'000'000'000ULL;
    manifest.sampleRate = 48'000U;
    manifest.applicationVersion = "0.4.1-test";
    manifest.mediaFormat = "opus 96000";
    manifest.state = TakeState::Recording;
    manifest.sources = {
        TakeSource{"src-1", "andrew", "instrument", "local-capture", "local-guitar.wav",
                   480'000U, 0U, ""},
        TakeSource{"src-2", "friend", "voice", "network-received", "remote-voice.wav",
                   480'000U, 1'440U, ""},
    };
    return manifest;
}

JAMLINK_TEST(a_manifest_survives_a_round_trip_intact) {
    const TakeManifest original = sampleTake();
    const auto parsed = parseTake(serialiseTake(original));
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->takeId == original.takeId);
    EXPECT_TRUE(parsed->sessionId == original.sessionId);
    EXPECT_TRUE(parsed->sampleRate == original.sampleRate);
    EXPECT_TRUE(parsed->mediaFormat == original.mediaFormat);
    EXPECT_TRUE(parsed->state == TakeState::Recording);
    EXPECT_TRUE(parsed->sources.size() == 2U);
    EXPECT_TRUE(parsed->sources[1].participantId == "friend");
    EXPECT_TRUE(parsed->sources[1].origin == "network-received");
    EXPECT_TRUE(parsed->sources[1].knownGapFrames == 1'440U);
}

JAMLINK_TEST(an_interrupted_take_is_never_presented_as_clean) {
    // The reason the journal exists. The recorder leaves playable files behind
    // when a process dies, and nothing in them says the ending is missing.
    const auto root = scratchRoot();
    const auto takeDirectory = root / "take-a";
    EXPECT_TRUE(TakeJournal::begin(takeDirectory, sampleTake()));

    // No finalise: this is what a crash leaves.
    const auto recovered = TakeJournal::recover(takeDirectory);
    EXPECT_TRUE(recovered.has_value());
    EXPECT_TRUE(recovered->state == TakeState::RecoveredNeedsReview);
    EXPECT_TRUE(!recovered->complete());
    EXPECT_TRUE(!recovered->interruptions.empty());

    // And the judgement is durable: reading it again does not quietly forget.
    const auto reread = TakeJournal::read(takeDirectory);
    EXPECT_TRUE(reread.has_value());
    EXPECT_TRUE(reread->state == TakeState::RecoveredNeedsReview);
}

JAMLINK_TEST(recovery_never_deletes_what_survived) {
    const auto root = scratchRoot();
    const auto takeDirectory = root / "take-b";
    EXPECT_TRUE(TakeJournal::begin(takeDirectory, sampleTake()));
    // Stand in for the audio the recorder had already written.
    {
        std::ofstream audio(takeDirectory / "local-guitar.wav", std::ios::binary);
        audio << "RIFF....WAVE";
    }
    static_cast<void>(TakeJournal::recover(takeDirectory));
    EXPECT_TRUE(std::filesystem::exists(takeDirectory / "local-guitar.wav"));
    EXPECT_TRUE(std::filesystem::exists(takeDirectory / TakeJournal::manifestFileName));
}

JAMLINK_TEST(only_a_successful_finalisation_reaches_ready) {
    const auto root = scratchRoot();
    const auto takeDirectory = root / "take-c";
    TakeManifest manifest = sampleTake();
    EXPECT_TRUE(TakeJournal::begin(takeDirectory, manifest));
    EXPECT_TRUE(TakeJournal::read(takeDirectory)->state == TakeState::Recording);

    manifest.endedAtMillisecond = manifest.startedAtMillisecond + 60'000ULL;
    EXPECT_TRUE(TakeJournal::finalise(takeDirectory, manifest));

    const auto finalised = TakeJournal::read(takeDirectory);
    EXPECT_TRUE(finalised->state == TakeState::Ready);
    EXPECT_TRUE(finalised->complete());
    // A finished take is left alone by recovery rather than being demoted.
    EXPECT_TRUE(TakeJournal::recover(takeDirectory)->state == TakeState::Ready);
}

JAMLINK_TEST(a_finished_take_still_admits_its_gaps) {
    // Complete and flawless are different claims. A take that finished cleanly
    // may still be missing frames the writer could not keep up with, and saying
    // so is the difference between an honest archive and a misleading one.
    const auto root = scratchRoot();
    const auto takeDirectory = root / "take-d";
    TakeManifest manifest = sampleTake();
    manifest.droppedFrames = 960U;
    EXPECT_TRUE(TakeJournal::begin(takeDirectory, manifest));
    EXPECT_TRUE(TakeJournal::finalise(takeDirectory, manifest));

    const auto finalised = TakeJournal::read(takeDirectory);
    EXPECT_TRUE(finalised->complete());
    EXPECT_TRUE(finalised->hasKnownGaps());
    EXPECT_TRUE(finalised->sources[1].knownGapFrames == 1'440U);
}

JAMLINK_TEST(a_half_written_manifest_cannot_replace_a_good_one) {
    // Replacement is atomic, so a process killed mid-write leaves either the
    // previous manifest or the new one. A partly written manifest would make a
    // real recording unreadable, which is worse than a stale one.
    const auto root = scratchRoot();
    const auto takeDirectory = root / "take-e";
    EXPECT_TRUE(TakeJournal::begin(takeDirectory, sampleTake()));

    // A leftover temporary from an interrupted write must not be mistaken for
    // the manifest.
    {
        std::ofstream partial(
            takeDirectory / (std::string(TakeJournal::manifestFileName) + ".new"),
            std::ios::binary);
        partial << "jamlink-take\t1\ntakeId\thalf-writ";
    }
    const auto manifest = TakeJournal::read(takeDirectory);
    EXPECT_TRUE(manifest.has_value());
    EXPECT_TRUE(manifest->takeId == "take-2026-08-17-2140");
    EXPECT_TRUE(manifest->sources.size() == 2U);
}

JAMLINK_TEST(startup_finds_the_takes_that_were_interrupted) {
    const auto root = scratchRoot();
    EXPECT_TRUE(TakeJournal::begin(root / "take-1", sampleTake()));

    TakeManifest finished = sampleTake();
    EXPECT_TRUE(TakeJournal::begin(root / "take-2", finished));
    EXPECT_TRUE(TakeJournal::finalise(root / "take-2", finished));

    EXPECT_TRUE(TakeJournal::begin(root / "take-3", sampleTake()));

    const auto interrupted = TakeJournal::findInterrupted(root);
    EXPECT_TRUE(interrupted.size() == 2U);
    EXPECT_TRUE(interrupted[0].filename() == "take-1");
    EXPECT_TRUE(interrupted[1].filename() == "take-3");

    // Once reviewed, a take stops being reported as interrupted, so a musician
    // is not asked about the same one every launch.
    static_cast<void>(TakeJournal::recover(root / "take-1"));
    EXPECT_TRUE(TakeJournal::findInterrupted(root).size() == 1U);
}

JAMLINK_TEST(a_state_this_build_does_not_understand_is_treated_as_unfinished) {
    // Forward compatibility that fails safe. A newer JamLink may write a state
    // this one has never heard of, and guessing Ready would be the one wrong
    // answer.
    const auto parsed = parseTake(
        "jamlink-take\t2\ntakeId\tfuture\nstate\tSomethingNewer\n");
    EXPECT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->state == TakeState::RecoveredNeedsReview);
    EXPECT_TRUE(!parsed->complete());
}

JAMLINK_TEST(text_that_is_not_a_manifest_is_refused) {
    EXPECT_TRUE(!parseTake("").has_value());
    EXPECT_TRUE(!parseTake("RIFF....WAVE").has_value());
    EXPECT_TRUE(!parseTake("takeId\tno-header\n").has_value());
}

JAMLINK_TEST(a_value_cannot_break_the_format_it_is_written_in) {
    TakeManifest manifest = sampleTake();
    manifest.interruptions = {"peer dropped\nstate\tReady"};
    const auto parsed = parseTake(serialiseTake(manifest));
    EXPECT_TRUE(parsed.has_value());
    // The injected line must not have become a field of its own.
    EXPECT_TRUE(parsed->state == TakeState::Recording);
    EXPECT_TRUE(parsed->interruptions.size() == 1U);
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
              << " take manifest tests passed\n";
    return failures == 0U ? 0 : 1;
}
