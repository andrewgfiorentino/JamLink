// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/record/session_recorder.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using jamlink::record::RecordTrack;
using jamlink::record::SessionRecorder;

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

constexpr std::uint32_t sampleRate = 48'000U;

[[nodiscard]] std::filesystem::path scratchRoot() {
    return std::filesystem::temp_directory_path() / "jamlink_recorder_tests";
}

struct WavFile final {
    std::uint16_t format{0U};
    std::uint16_t channels{0U};
    std::uint32_t sampleRate{0U};
    std::uint16_t bitsPerSample{0U};
    std::uint32_t declaredDataBytes{0U};
    std::uint32_t declaredSampleFrames{0U};
    std::vector<float> samples;
    bool valid{false};
};

[[nodiscard]] std::uint32_t readU32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
        value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint16_t readU16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(
        static_cast<std::uint16_t>(bytes[offset])
        | static_cast<std::uint16_t>(static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] bool tagAt(
    const std::vector<std::uint8_t>& bytes, std::size_t offset, const char* tag) {
    for (std::size_t index = 0U; index < 4U; ++index) {
        if (bytes[offset + index] != static_cast<std::uint8_t>(tag[index])) {
            return false;
        }
    }
    return true;
}

// Parses the file the way an unrelated tool would, so the test proves the
// output is a real WAV rather than only round-tripping our own writer.
[[nodiscard]] WavFile readWav(const std::filesystem::path& path) {
    WavFile wav;
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return wav;
    }
    // Braces, because the parenthesised form is a function declaration.
    const std::vector<std::uint8_t> bytes{
        std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.size() < 58U || !tagAt(bytes, 0U, "RIFF") || !tagAt(bytes, 8U, "WAVE")
        || !tagAt(bytes, 12U, "fmt ") || !tagAt(bytes, 38U, "fact")
        || !tagAt(bytes, 50U, "data")) {
        return wav;
    }
    if (readU32(bytes, 4U) != bytes.size() - 8U) {
        return wav;
    }
    wav.format = readU16(bytes, 20U);
    wav.channels = readU16(bytes, 22U);
    wav.sampleRate = readU32(bytes, 24U);
    wav.bitsPerSample = readU16(bytes, 34U);
    wav.declaredSampleFrames = readU32(bytes, 46U);
    wav.declaredDataBytes = readU32(bytes, 54U);
    if (wav.declaredDataBytes != bytes.size() - 58U) {
        return wav;
    }
    wav.samples.resize(wav.declaredDataBytes / sizeof(float));
    if (!wav.samples.empty()) {
        std::memcpy(wav.samples.data(), bytes.data() + 58U, wav.declaredDataBytes);
    }
    wav.valid = true;
    return wav;
}

[[nodiscard]] std::vector<float> ramp(std::size_t frames, float scale) {
    std::vector<float> signal(frames, 0.0F);
    for (std::size_t index = 0U; index < frames; ++index) {
        signal[index] = static_cast<float>(index) * scale;
    }
    return signal;
}

// Waits for the disk worker to catch up without assuming a fixed latency.
void waitForFrames(const SessionRecorder& recorder, std::uint64_t frames) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline
           && recorder.telemetry().framesWritten < frames) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

JAMLINK_TEST(recorder_writes_one_readable_wav_per_track) {
    const auto root = scratchRoot() / "basic";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);

    SessionRecorder recorder;
    EXPECT_TRUE(recorder.start(root, "take", sampleRate));
    EXPECT_TRUE(recorder.recording());

    constexpr std::size_t frames = 4'800U;
    const auto instrument = ramp(frames, 0.0001F);
    const auto voice = ramp(frames, -0.0001F);
    recorder.write(RecordTrack::LocalInstrument, instrument);
    recorder.write(RecordTrack::LocalVoice, voice);
    recorder.write(RecordTrack::RemoteInstrument, instrument);
    recorder.write(RecordTrack::RemoteVoice, voice);
    recorder.write(RecordTrack::LocalInstrumentOriginal, instrument);
    recorder.write(RecordTrack::LocalVoiceOriginal, voice);
    waitForFrames(recorder, frames);
    recorder.stop();
    EXPECT_TRUE(!recorder.recording());

    const auto session = root / "take";
    for (std::size_t index = 0U; index < jamlink::record::recordTrackCount; ++index) {
        const auto track = static_cast<RecordTrack>(index);
        const auto wav = readWav(session / SessionRecorder::trackFileName(track));
        EXPECT_TRUE(wav.valid);
        EXPECT_TRUE(wav.format == 3U);
        EXPECT_TRUE(wav.channels == 1U);
        EXPECT_TRUE(wav.sampleRate == sampleRate);
        EXPECT_TRUE(wav.bitsPerSample == 32U);
        EXPECT_TRUE(wav.samples.size() == frames);
        EXPECT_TRUE(wav.declaredSampleFrames == frames);
    }

    // Sample values must survive exactly; this is a lossless local master.
    const auto written = readWav(session / "instrument.wav");
    for (std::size_t index = 0U; index < frames; ++index) {
        EXPECT_TRUE(written.samples[index] == instrument[index]);
    }
    const auto voiceWritten = readWav(session / "voice.wav");
    for (std::size_t index = 0U; index < frames; ++index) {
        EXPECT_TRUE(voiceWritten.samples[index] == voice[index]);
    }
    std::cout << "    wrote " << frames << " frames to "
              << jamlink::record::recordTrackCount << " tracks\n";
}

JAMLINK_TEST(recorder_keeps_tracks_sample_aligned) {
    const auto root = scratchRoot() / "aligned";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);

    SessionRecorder recorder;
    EXPECT_TRUE(recorder.start(root, "take", sampleRate));

    // The render callback hands every track the same frame count each block,
    // using silence where a source has nothing to contribute.
    constexpr std::size_t blocks = 40U;
    constexpr std::size_t blockFrames = 128U;
    const std::vector<float> silence(blockFrames, 0.0F);
    const auto tone = ramp(blockFrames, 0.001F);
    for (std::size_t block = 0U; block < blocks; ++block) {
        recorder.write(RecordTrack::LocalInstrument, tone);
        recorder.write(RecordTrack::LocalVoice, silence);
        // The friend joins late, which must not shift their track earlier.
        recorder.write(
            RecordTrack::RemoteInstrument, block < 10U ? silence : tone);
        recorder.write(RecordTrack::RemoteVoice, silence);
    }
    waitForFrames(recorder, blocks * blockFrames);
    recorder.stop();

    const auto session = root / "take";
    const auto local = readWav(session / "instrument.wav");
    const auto remote = readWav(session / "friend-instrument.wav");
    EXPECT_TRUE(local.valid && remote.valid);
    EXPECT_TRUE(local.samples.size() == blocks * blockFrames);
    EXPECT_TRUE(remote.samples.size() == local.samples.size());
    // The friend's audio has to land at block 10, not at the start.
    EXPECT_TRUE(remote.samples[9U * blockFrames + 5U] == 0.0F);
    EXPECT_TRUE(remote.samples[10U * blockFrames + 5U] == tone[5U]);
}

JAMLINK_TEST(recorder_write_allocates_nothing_on_the_audio_thread) {
    const auto root = scratchRoot() / "realtime";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);

    SessionRecorder recorder;
    EXPECT_TRUE(recorder.start(root, "take", sampleRate));
    const auto block = ramp(128U, 0.001F);
    recorder.write(RecordTrack::LocalInstrument, block);

    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_relaxed);
    for (std::size_t round = 0U; round < 400U; ++round) {
        recorder.write(RecordTrack::LocalInstrument, block);
        recorder.write(RecordTrack::LocalVoice, block);
        recorder.write(RecordTrack::RemoteInstrument, block);
        recorder.write(RecordTrack::RemoteVoice, block);
    }
    allocationTrackingEnabled.store(false, std::memory_order_relaxed);
    const auto allocations = trackedAllocationCount.load(std::memory_order_relaxed);

    recorder.stop();
    EXPECT_TRUE(allocations == 0U);
}

JAMLINK_TEST(recorder_reports_dropped_frames_instead_of_blocking) {
    const auto root = scratchRoot() / "overrun";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);

    // A ring far smaller than the burst below, so the disk worker cannot keep
    // up. The audio callback must shed frames and say so, never wait.
    SessionRecorder recorder(1U << 10U);
    EXPECT_TRUE(recorder.start(root, "take", sampleRate));
    const auto block = ramp(1'024U, 0.0005F);
    for (std::size_t round = 0U; round < 64U; ++round) {
        recorder.write(RecordTrack::LocalInstrument, block);
    }
    const auto telemetry = recorder.telemetry();
    recorder.stop();

    EXPECT_TRUE(telemetry.droppedFrames > 0U);
    std::cout << "    shed " << telemetry.droppedFrames
              << " frames under a deliberately undersized ring\n";
    // The file still has to be valid, just short.
    const auto wav = readWav(root / "take" / "instrument.wav");
    EXPECT_TRUE(wav.valid);
}

JAMLINK_TEST(recorder_leaves_a_playable_file_if_the_process_never_stops) {
    const auto root = scratchRoot() / "crash";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);

    auto recorder = std::make_unique<SessionRecorder>();
    EXPECT_TRUE(recorder->start(root, "take", sampleRate));
    constexpr std::size_t frames = 9'600U;
    const auto block = ramp(frames, 0.00005F);
    recorder->write(RecordTrack::LocalInstrument, block);
    waitForFrames(*recorder, frames);

    // Read the file while recording is still running, which is what a crashed
    // process would leave behind after the periodic size refresh.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    WavFile wav;
    while (std::chrono::steady_clock::now() < deadline && !wav.valid) {
        wav = readWav(root / "take" / "instrument.wav");
        if (!wav.valid) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    recorder->stop();

    EXPECT_TRUE(wav.valid);
    EXPECT_TRUE(wav.samples.size() == frames);
    std::cout << "    mid-session file was already playable with "
              << wav.samples.size() << " frames\n";
}

JAMLINK_TEST(recorder_rejects_bad_configuration_and_double_start) {
    const auto root = scratchRoot() / "config";
    std::error_code cleanup;
    std::filesystem::remove_all(root, cleanup);

    SessionRecorder recorder;
    EXPECT_TRUE(!recorder.start(root, "", sampleRate));
    EXPECT_TRUE(!recorder.start(root, "take", 100U));
    EXPECT_TRUE(!recorder.recording());

    EXPECT_TRUE(recorder.start(root, "take", sampleRate));
    EXPECT_TRUE(!recorder.start(root, "another", sampleRate));
    recorder.stop();
    // Stopping twice is harmless.
    recorder.stop();
    EXPECT_TRUE(!recorder.recording());
}

} // namespace

JAMLINK_TEST(local_originals_keep_their_own_capture_rate) {
    // An original resampled to the timeline's rate is no longer an original.
    // Capture devices need not agree with the output device or with each
    // other, so each original keeps the rate it was actually captured at and
    // says so in its own header -- otherwise it would play back at the wrong
    // pitch, which is a subtle enough failure to survive a listening test.
    const auto root = scratchRoot();
    SessionRecorder recorder;
    EXPECT_TRUE(recorder.start(root, "rates", 48'000U, 44'100U, 96'000U));

    constexpr std::size_t frames = 2'400U;
    const auto block = ramp(frames, 0.0001F);
    for (std::size_t index = 0U; index < jamlink::record::recordTrackCount; ++index) {
        recorder.write(static_cast<RecordTrack>(index), block);
    }
    waitForFrames(recorder, frames);
    recorder.stop();

    const auto session = root / "rates";
    const auto timeline = readWav(
        session / SessionRecorder::trackFileName(RecordTrack::LocalInstrument));
    const auto guitarOriginal = readWav(
        session / SessionRecorder::trackFileName(RecordTrack::LocalInstrumentOriginal));
    const auto voiceOriginal = readWav(
        session / SessionRecorder::trackFileName(RecordTrack::LocalVoiceOriginal));

    EXPECT_TRUE(timeline.valid && guitarOriginal.valid && voiceOriginal.valid);
    EXPECT_TRUE(timeline.sampleRate == 48'000U);
    EXPECT_TRUE(guitarOriginal.sampleRate == 44'100U);
    EXPECT_TRUE(voiceOriginal.sampleRate == 96'000U);
    // And the samples are still exact: an original is lossless or it is not an
    // original.
    EXPECT_TRUE(guitarOriginal.samples.size() == frames);
    for (std::size_t index = 0U; index < frames; ++index) {
        EXPECT_TRUE(guitarOriginal.samples[index] == block[index]);
    }
}

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
    std::error_code cleanup;
    std::filesystem::remove_all(scratchRoot(), cleanup);
    std::cout << (tests().size() - failures) << '/' << tests().size()
              << " recorder tests passed\n";
    return failures == 0U ? 0 : 1;
}
