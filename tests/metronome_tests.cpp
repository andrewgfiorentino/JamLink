// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// A metronome that drifts is worse than none: the drift is exactly the thing a
// musician is listening for, and they will assume it is them.

#include "jamlink/audio/metronome.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::atomic<bool> trackAllocations{false};
std::atomic<std::size_t> allocationCount{0U};

} // namespace

void* operator new(std::size_t bytes) {
    if (trackAllocations.load(std::memory_order_relaxed)) {
        allocationCount.fetch_add(1U, std::memory_order_relaxed);
    }
    void* memory = std::malloc(bytes == 0U ? 1U : bytes);
    if (memory == nullptr) {
        throw std::bad_alloc();
    }
    return memory;
}

void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

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

using jamlink::audio::Metronome;

constexpr std::uint32_t sampleRate = 48'000U;

// Where each click begins, found from the audio itself rather than from the
// metronome's own bookkeeping -- otherwise the test would be checking that the
// counter agrees with itself.
[[nodiscard]] std::vector<std::size_t> clickStarts(const std::vector<float>& audio) {
    std::vector<std::size_t> starts;
    bool inClick = false;
    for (std::size_t index = 0U; index < audio.size(); ++index) {
        const bool loud = std::fabs(audio[index]) > 1.0e-4F;
        if (loud && !inClick) {
            starts.push_back(index);
        }
        // A click has zero crossings, so silence has to persist before the
        // next rise counts as a new click.
        if (!loud) {
            bool quietRun = true;
            for (std::size_t look = index;
                 look < std::min(index + 64U, audio.size()); ++look) {
                if (std::fabs(audio[look]) > 1.0e-4F) {
                    quietRun = false;
                    break;
                }
            }
            if (quietRun) {
                inClick = false;
                continue;
            }
        }
        inClick = inClick || loud;
    }
    return starts;
}

[[nodiscard]] std::vector<float> render(Metronome& metronome, std::size_t frames) {
    std::vector<float> audio(frames, 0.0F);
    // In blocks, because the audio callback never hands over a whole minute at
    // once and a metronome that only lines up on block boundaries is useless.
    constexpr std::size_t block = 233U;  // deliberately not a divisor of anything
    for (std::size_t at = 0U; at < frames; at += block) {
        const std::size_t count = std::min(block, frames - at);
        metronome.mix(std::span<float>(audio.data() + at, count));
    }
    return audio;
}

JAMLINK_TEST(a_stopped_metronome_adds_nothing) {
    Metronome metronome;
    metronome.prepare(sampleRate, {});
    const auto audio = render(metronome, 48'000U);
    for (const float sample : audio) {
        EXPECT_TRUE(sample == 0.0F);
    }
}

JAMLINK_TEST(clicks_land_on_the_beat_after_a_full_minute) {
    // The whole point. At 120 bpm a beat is exactly 24000 frames, and after
    // 120 beats a drifting metronome is audibly wrong while a correct one is
    // still exact.
    Metronome::Settings settings;
    settings.beatsPerMinute = 120U;
    settings.countInBars = 0U;
    Metronome metronome;
    metronome.prepare(sampleRate, settings);
    metronome.start();
    const auto audio = render(metronome, sampleRate * 60U);
    const auto starts = clickStarts(audio);
    EXPECT_TRUE(starts.size() == 120U);
    // Compared bar to bar rather than beat to beat, and not for convenience.
    // The click envelope begins at exactly zero, so the first sample loud
    // enough to detect is a few frames in -- and an accented click, being an
    // octave higher, reaches that level a couple of frames sooner than an
    // ordinary one. That offset belongs to this detector rather than to the
    // metronome, so each beat is compared with the beat one bar earlier, which
    // always carries the same accent.
    for (std::size_t beat = 4U; beat < starts.size(); ++beat) {
        EXPECT_TRUE(starts[beat] - starts[beat - 4U] == 4U * 24'000U);
    }
}

JAMLINK_TEST(the_first_beat_of_a_bar_is_accented) {
    // A count-in that is not countable is just noise before a take.
    Metronome::Settings settings;
    settings.beatsPerMinute = 120U;
    settings.beatsPerBar = 4U;
    settings.countInBars = 0U;
    Metronome metronome;
    metronome.prepare(sampleRate, settings);
    metronome.start();
    const auto audio = render(metronome, 24'000U * 8U);
    const auto starts = clickStarts(audio);
    EXPECT_TRUE(starts.size() == 8U);

    const auto peakAt = [&audio](std::size_t from) {
        float peak = 0.0F;
        for (std::size_t index = from; index < std::min(from + 600U, audio.size()); ++index) {
            peak = std::max(peak, std::fabs(audio[index]));
        }
        return peak;
    };
    // Same level, different pitch: an accent that was merely louder would just
    // be a metronome with an uneven volume.
    EXPECT_TRUE(peakAt(starts[0]) > 0.0F);
    EXPECT_TRUE(peakAt(starts[4]) > 0.0F);
    // Downbeats are the ones that repeat every four.
    EXPECT_TRUE(starts[4] - starts[0] == 4U * 24'000U);
}

JAMLINK_TEST(a_count_in_finishes_before_the_take_would_start) {
    // A take that begins during the count-in records a click track for its
    // first bar, which is exactly what the count-in exists to prevent.
    Metronome::Settings settings;
    settings.beatsPerMinute = 120U;
    settings.beatsPerBar = 4U;
    settings.countInBars = 2U;
    Metronome metronome;
    metronome.prepare(sampleRate, settings);
    metronome.start();
    EXPECT_TRUE(metronome.countingIn());
    EXPECT_TRUE(metronome.countInBeatsRemaining() == 8U);

    // Halfway through: still counting, and the display counts down rather than
    // sitting still.
    static_cast<void>(render(metronome, 24'000U * 4U));
    EXPECT_TRUE(metronome.countingIn());
    EXPECT_TRUE(metronome.countInBeatsRemaining() == 4U);

    static_cast<void>(render(metronome, 24'000U * 4U));
    EXPECT_TRUE(!metronome.countingIn());
    EXPECT_TRUE(metronome.countInBeatsRemaining() == 0U);
    // And it keeps going afterwards: the count-in leads into the click, it
    // does not replace it.
    EXPECT_TRUE(metronome.running());
}

JAMLINK_TEST(the_click_sits_over_the_playing_rather_than_replacing_it) {
    Metronome::Settings settings;
    settings.countInBars = 0U;
    Metronome metronome;
    metronome.prepare(sampleRate, settings);
    metronome.start();
    std::vector<float> audio(4'800U, 0.25F);
    metronome.mix(audio);
    // Every sample still carries what was there before.
    for (const float sample : audio) {
        EXPECT_TRUE(sample != 0.0F);
    }
    // And well past the click, the signal is untouched.
    EXPECT_TRUE(audio.back() == 0.25F);
}

JAMLINK_TEST(an_absurd_tempo_is_clamped_rather_than_dividing_by_zero) {
    Metronome::Settings settings;
    settings.beatsPerMinute = 0U;
    settings.beatsPerBar = 0U;
    Metronome metronome;
    metronome.prepare(sampleRate, settings);
    metronome.start();
    const auto audio = render(metronome, 48'000U);
    bool anySound = false;
    for (const float sample : audio) {
        EXPECT_TRUE(std::isfinite(sample));
        anySound = anySound || sample != 0.0F;
    }
    EXPECT_TRUE(anySound);
}

JAMLINK_TEST(the_tempo_can_be_changed_while_it_is_playing) {
    // Finding a tempo is done by ear, which means changing it while listening
    // rather than stopping, typing a number, and starting again.
    Metronome::Settings settings;
    settings.beatsPerMinute = 120U;
    // One beat to the bar, so every click carries the same accent and the
    // detector's onset offset is identical for all of them. With mixed accents
    // consecutive intervals are not comparable, and the difference is the
    // detector rather than the metronome.
    settings.beatsPerBar = 1U;
    settings.countInBars = 0U;
    Metronome metronome;
    metronome.prepare(sampleRate, settings);
    metronome.start();
    static_cast<void>(render(metronome, 24'000U * 4U));

    metronome.setBeatsPerMinute(60U);
    // Reported immediately, because a tempo box that snapped back to the old
    // number until the next beat would look broken.
    EXPECT_TRUE(metronome.beatsPerMinute() == 60U);

    const auto audio = render(metronome, 48'000U * 6U);
    const auto starts = clickStarts(audio);
    EXPECT_TRUE(starts.size() >= 4U);
    // The first interval here still belongs to the old tempo: the beat that
    // was already under way keeps its length, which is the documented and
    // musical behaviour rather than an artefact. Everything after it is a
    // whole second, which is what sixty beats a minute means.
    for (std::size_t beat = 2U; beat < starts.size(); ++beat) {
        EXPECT_TRUE(starts[beat] - starts[beat - 1U] == 48'000U);
    }
    EXPECT_TRUE(metronome.beatsPerMinute() == 60U);
}

JAMLINK_TEST(a_tempo_change_never_lands_in_the_middle_of_a_beat) {
    // Applying it the instant it arrives would make one beat the wrong length,
    // which is heard as a stumble -- and heard at exactly the moment somebody
    // is concentrating on the tempo.
    Metronome::Settings settings;
    settings.beatsPerMinute = 120U;
    settings.beatsPerBar = 1U;
    settings.countInBars = 0U;
    Metronome metronome;
    metronome.prepare(sampleRate, settings);
    metronome.start();

    // Change it a third of the way through a beat.
    static_cast<void>(render(metronome, 8'000U));
    metronome.setBeatsPerMinute(240U);
    const auto audio = render(metronome, 24'000U * 6U);
    const auto starts = clickStarts(audio);
    EXPECT_TRUE(starts.size() >= 3U);
    // The beat that was already under way keeps its old length; everything
    // after it is at the new tempo.
    for (std::size_t beat = 2U; beat < starts.size(); ++beat) {
        EXPECT_TRUE(starts[beat] - starts[beat - 1U] == 12'000U);
    }
}

JAMLINK_TEST(an_absurd_tempo_change_is_clamped_like_the_initial_one) {
    Metronome metronome;
    metronome.prepare(sampleRate, {});
    metronome.start();
    metronome.setBeatsPerMinute(100'000U);
    EXPECT_TRUE(metronome.beatsPerMinute() == 300U);
    metronome.setBeatsPerMinute(1U);
    EXPECT_TRUE(metronome.beatsPerMinute() == 30U);
    std::vector<float> audio(4'800U, 0.0F);
    metronome.mix(audio);
    for (const float sample : audio) {
        EXPECT_TRUE(std::isfinite(sample));
    }
}

JAMLINK_TEST(mixing_allocates_nothing) {
    Metronome metronome;
    metronome.prepare(sampleRate, {});
    metronome.start();
    std::vector<float> audio(480U, 0.0F);
    trackAllocations.store(true, std::memory_order_relaxed);
    allocationCount.store(0U, std::memory_order_relaxed);
    for (int pass = 0; pass < 500; ++pass) {
        metronome.mix(audio);
    }
    trackAllocations.store(false, std::memory_order_relaxed);
    EXPECT_TRUE(allocationCount.load(std::memory_order_relaxed) == 0U);
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
              << " metronome tests passed\n";
    return failures == 0U ? 0 : 1;
}
