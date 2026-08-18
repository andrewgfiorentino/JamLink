// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// The last thing between a hot input and a friend's headphones. It has to be
// inaudible when it is not needed, which is almost always, and it has to be
// exact when it is.

#include "jamlink/audio/send_limiter.hpp"

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <numbers>
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

using jamlink::audio::SendLimiter;

constexpr std::uint32_t sampleRate = 48'000U;

[[nodiscard]] std::vector<float> tone(std::size_t frames, double amplitude, double hertz) {
    std::vector<float> samples(frames, 0.0F);
    for (std::size_t index = 0U; index < frames; ++index) {
        const double phase = 2.0 * std::numbers::pi * hertz
            * static_cast<double>(index) / static_cast<double>(sampleRate);
        samples[index] = static_cast<float>(amplitude * std::sin(phase));
    }
    return samples;
}

JAMLINK_TEST(ordinary_playing_comes_out_bit_identical) {
    // The whole design rests on this. A limiter that altered quiet playing
    // would be colouring the instrument every second of every session in
    // exchange for nothing, and it would do so where nobody would think to
    // look for it.
    SendLimiter limiter(sampleRate);
    const auto source = tone(4'800U, 0.7, 220.0);
    auto block = source;
    limiter.process(block);
    for (std::size_t index = 0U; index < source.size(); ++index) {
        EXPECT_TRUE(block[index] == source[index]);
    }
    EXPECT_TRUE(!limiter.engaged());
    EXPECT_TRUE(limiter.deepestGain() == 1.0F);
}

JAMLINK_TEST(nothing_ever_leaves_above_the_threshold) {
    SendLimiter limiter(sampleRate);
    // Well past full scale, which is what a badly set interface gain produces.
    auto block = tone(4'800U, 3.5, 110.0);
    limiter.process(block);
    for (const float sample : block) {
        // Not "roughly": a limiter that overshoots even once has failed at the
        // one thing it exists for.
        EXPECT_TRUE(std::fabs(sample) <= SendLimiter::defaultThreshold);
    }
    EXPECT_TRUE(limiter.engaged());
    EXPECT_TRUE(limiter.deepestGain() < 1.0F);
}

JAMLINK_TEST(a_single_transient_is_caught_on_its_very_first_sample) {
    // No lookahead, so this is the case that would fail if the gain were ever
    // computed from the samples before it: the first sample of a transient
    // would escape at full amplitude.
    SendLimiter limiter(sampleRate);
    std::vector<float> block(64U, 0.0F);
    block[10U] = 4.0F;
    limiter.process(block);
    EXPECT_TRUE(std::fabs(block[10U]) <= SendLimiter::defaultThreshold);
    // And the samples before it were silent and stayed silent.
    for (std::size_t index = 0U; index < 10U; ++index) {
        EXPECT_TRUE(block[index] == 0.0F);
    }
}

JAMLINK_TEST(the_note_after_a_transient_is_not_left_ducked) {
    // A limiter that recovers too slowly turns one loud pick into a hole in
    // the next bar, which is more noticeable than the over it prevented.
    SendLimiter limiter(sampleRate);
    std::vector<float> spike(16U, 0.0F);
    spike[0] = 6.0F;
    limiter.process(spike);
    EXPECT_TRUE(limiter.deepestGain() < 0.2F);

    // Recovery is a one-pole toward unity, so "transparent again" arrives in
    // two stages and both are worth pinning. A fifth of a second is four time
    // constants, which from fifteen decibels down leaves under two percent of
    // attenuation -- about a sixth of a decibel, and inaudible. A tighter
    // release than this would recover sooner but would modulate at the period
    // of a low string, which trades an inaudible swell for audible distortion.
    const auto source = tone(9'600U, 0.5, 196.0);
    auto block = source;
    limiter.process(block);
    const std::size_t tail = block.size() - 480U;
    for (std::size_t index = tail; index < block.size(); ++index) {
        EXPECT_TRUE(std::fabs(block[index] - source[index])
            <= 0.02F * std::fabs(source[index]) + 1.0e-6F);
    }

    // And within a second it is bit-exact again, not merely close: the
    // limiter has to get entirely out of the way, or every session would run
    // slightly attenuated after the first loud note of the night.
    const auto later = tone(48'000U, 0.5, 196.0);
    auto lateBlock = later;
    limiter.process(lateBlock);
    const std::size_t lateTail = lateBlock.size() - 480U;
    for (std::size_t index = lateTail; index < lateBlock.size(); ++index) {
        EXPECT_TRUE(lateBlock[index] == later[index]);
    }
}

JAMLINK_TEST(sustained_overs_are_held_down_without_pumping) {
    // A guitarist digging in for a whole chorus. The output stays inside the
    // threshold throughout, and the gain does not oscillate back to unity
    // between samples, which is what would make it audible as pumping.
    SendLimiter limiter(sampleRate);
    auto block = tone(48'000U, 1.6, 82.4);
    limiter.process(block);
    for (const float sample : block) {
        EXPECT_TRUE(std::fabs(sample) <= SendLimiter::defaultThreshold);
    }
    // Every sample above the threshold was counted, and on a signal this hot
    // that is a large fraction of them rather than a handful.
    EXPECT_TRUE(limiter.limitedSamples() > 10'000U);
}

JAMLINK_TEST(reset_returns_it_to_a_transparent_state) {
    SendLimiter limiter(sampleRate);
    std::vector<float> spike(8U, 5.0F);
    limiter.process(spike);
    EXPECT_TRUE(limiter.engaged());
    limiter.reset();
    EXPECT_TRUE(!limiter.engaged());
    EXPECT_TRUE(limiter.deepestGain() == 1.0F);

    const auto source = tone(480U, 0.6, 440.0);
    auto block = source;
    limiter.process(block);
    for (std::size_t index = 0U; index < source.size(); ++index) {
        EXPECT_TRUE(block[index] == source[index]);
    }
}

JAMLINK_TEST(processing_allocates_nothing) {
    SendLimiter limiter(sampleRate);
    auto quiet = tone(960U, 0.4, 330.0);
    auto loud = tone(960U, 2.0, 330.0);
    trackAllocations.store(true, std::memory_order_relaxed);
    allocationCount.store(0U, std::memory_order_relaxed);
    for (int pass = 0; pass < 200; ++pass) {
        limiter.process(quiet);
        limiter.process(loud);
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
              << " send limiter tests passed\n";
    return failures == 0U ? 0 : 1;
}
