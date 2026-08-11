// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/instrument_tuner.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iomanip>
#include <iostream>
#include <new>
#include <numbers>
#include <stdexcept>
#include <string>
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

using jamlink::audio::InstrumentTuner;
using jamlink::audio::TunerReading;

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

// A plucked string is not a sine. The first harmonic often exceeds the
// fundamental, which is exactly the case that defeats naive autocorrelation, so
// the test signal is built that way on purpose.
[[nodiscard]] std::vector<float> pluckedString(
    double fundamental,
    std::size_t frames,
    double amplitude = 0.5,
    double phase = 0.0) {
    static constexpr double partials[] = {0.6, 1.0, 0.55, 0.30, 0.18, 0.10, 0.06};
    std::vector<float> signal(frames, 0.0F);
    for (std::size_t index = 0U; index < frames; ++index) {
        const double time = static_cast<double>(index) / static_cast<double>(sampleRate);
        double value = 0.0;
        for (std::size_t partial = 0U; partial < std::size(partials); ++partial) {
            const double harmonic = static_cast<double>(partial + 1U);
            value += partials[partial]
                * std::sin(2.0 * std::numbers::pi * fundamental * harmonic * time
                           + phase * harmonic);
        }
        signal[index] = static_cast<float>(value * amplitude / 2.8);
    }
    return signal;
}

[[nodiscard]] TunerReading readSignal(InstrumentTuner& tuner, std::span<const float> signal) {
    // Feed in callback-sized blocks, as the audio path would.
    constexpr std::size_t block = 128U;
    for (std::size_t offset = 0U; offset < signal.size(); offset += block) {
        const std::size_t count = std::min(block, signal.size() - offset);
        tuner.write(signal.subspan(offset, count), sampleRate);
    }
    return tuner.analyse();
}

[[nodiscard]] double centsFromHz(double measured, double truth) {
    return 1'200.0 * std::log2(measured / truth);
}

JAMLINK_TEST(tuner_reports_nothing_without_signal) {
    InstrumentTuner tuner;
    EXPECT_TRUE(!tuner.analyse().detected);

    const std::vector<float> silence(InstrumentTuner::windowFrames * 2U, 0.0F);
    const TunerReading reading = readSignal(tuner, silence);
    EXPECT_TRUE(!reading.detected);
    EXPECT_TRUE(reading.level < 0.001F);
}

JAMLINK_TEST(tuner_tracks_standard_guitar_tuning_within_a_cent) {
    // Standard tuning at A4 = 440 Hz, plus a low B and a high fretted note.
    struct Target final {
        const char* name;
        double frequency;
        int midiNote;
    };
    static constexpr Target targets[] = {
        {"B0", 30.868, 23},
        {"E2", 82.407, 40},
        {"A2", 110.000, 45},
        {"D3", 146.832, 50},
        {"G3", 195.998, 55},
        {"B3", 246.942, 59},
        {"E4", 329.628, 64},
        {"A4", 440.000, 69},
        {"E6", 1'318.510, 88},
    };

    double worstCents = 0.0;
    for (const Target& target : targets) {
        InstrumentTuner tuner;
        const auto signal = pluckedString(target.frequency, InstrumentTuner::windowFrames * 2U);
        const TunerReading reading = readSignal(tuner, signal);
        EXPECT_TRUE(reading.detected);
        EXPECT_TRUE(reading.midiNote == target.midiNote);
        const double error = std::abs(centsFromHz(reading.frequency, target.frequency));
        worstCents = std::max(worstCents, error);
        std::cout << "    " << std::setw(3) << target.name << " "
                  << std::fixed << std::setprecision(3) << reading.frequency
                  << " Hz, error " << std::setprecision(2) << error
                  << " cents, clarity " << reading.clarity << "\n";
        // A tuner that is wrong by more than a cent is not worth trusting.
        EXPECT_TRUE(error < 1.0);
        EXPECT_TRUE(std::abs(reading.cents) < 1.0);
    }
    std::cout << "    worst error across the range: " << std::setprecision(2)
              << worstCents << " cents\n";
}

JAMLINK_TEST(tuner_reports_signed_cents_for_detuned_strings) {
    // Fifteen cents flat and sharp of A2, the classic "nearly there" case.
    const double flat = 110.0 * std::pow(2.0, -15.0 / 1'200.0);
    const double sharp = 110.0 * std::pow(2.0, 15.0 / 1'200.0);

    InstrumentTuner flatTuner;
    const TunerReading flatReading = readSignal(
        flatTuner, pluckedString(flat, InstrumentTuner::windowFrames * 2U));
    EXPECT_TRUE(flatReading.detected);
    EXPECT_TRUE(flatReading.midiNote == 45);
    EXPECT_TRUE(flatReading.cents < -14.0 && flatReading.cents > -16.0);

    InstrumentTuner sharpTuner;
    const TunerReading sharpReading = readSignal(
        sharpTuner, pluckedString(sharp, InstrumentTuner::windowFrames * 2U));
    EXPECT_TRUE(sharpReading.detected);
    EXPECT_TRUE(sharpReading.midiNote == 45);
    EXPECT_TRUE(sharpReading.cents > 14.0 && sharpReading.cents < 16.0);
}

JAMLINK_TEST(tuner_follows_the_reference_pitch) {
    InstrumentTuner tuner;
    tuner.setReferenceHz(432.0);
    // 432 Hz becomes A4 exactly when the reference moves.
    const TunerReading reading = readSignal(
        tuner, pluckedString(432.0, InstrumentTuner::windowFrames * 2U));
    EXPECT_TRUE(reading.detected);
    EXPECT_TRUE(reading.midiNote == 69);
    EXPECT_TRUE(std::abs(reading.cents) < 1.0);

    // The same signal read against concert pitch is a third of a semitone flat.
    InstrumentTuner concert;
    const TunerReading concertReading = readSignal(
        concert, pluckedString(432.0, InstrumentTuner::windowFrames * 2U));
    EXPECT_TRUE(concertReading.detected);
    EXPECT_TRUE(concertReading.midiNote == 69);
    EXPECT_TRUE(concertReading.cents < -30.0);
}

JAMLINK_TEST(tuner_is_not_fooled_by_a_missing_fundamental) {
    // Strings recorded through small speakers or thin pickups often lose the
    // fundamental entirely. The pitch is still E2.
    constexpr double fundamental = 82.407;
    std::vector<float> signal(InstrumentTuner::windowFrames * 2U, 0.0F);
    for (std::size_t index = 0U; index < signal.size(); ++index) {
        const double time = static_cast<double>(index) / static_cast<double>(sampleRate);
        double value = 0.0;
        for (int harmonic = 2; harmonic <= 7; ++harmonic) {
            value += std::sin(
                2.0 * std::numbers::pi * fundamental * harmonic * time) / harmonic;
        }
        signal[index] = static_cast<float>(value * 0.3);
    }

    InstrumentTuner tuner;
    const TunerReading reading = readSignal(tuner, signal);
    EXPECT_TRUE(reading.detected);
    EXPECT_TRUE(reading.midiNote == 40);
    std::cout << "    missing fundamental: " << std::fixed << std::setprecision(3)
              << reading.frequency << " Hz, error " << std::setprecision(2)
              << std::abs(centsFromHz(reading.frequency, fundamental)) << " cents\n";
    EXPECT_TRUE(std::abs(centsFromHz(reading.frequency, fundamental)) < 5.0);
}

JAMLINK_TEST(tuner_survives_noise_and_reports_clarity) {
    // Deterministic pseudo-noise mixed under the string.
    std::uint64_t state = 0x2545F4914F6CDD1DULL;
    const auto noise = [&state]() {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        return static_cast<float>(static_cast<std::int64_t>(state >> 11U)
            / static_cast<double>(1ULL << 52U)) - 0.5F;
    };

    auto signal = pluckedString(146.832, InstrumentTuner::windowFrames * 2U, 0.5);
    for (float& sample : signal) {
        sample += noise() * 0.05F;
    }

    InstrumentTuner tuner;
    const TunerReading reading = readSignal(tuner, signal);
    EXPECT_TRUE(reading.detected);
    EXPECT_TRUE(reading.midiNote == 50);
    EXPECT_TRUE(reading.clarity > 0.6F);
    std::cout << "    with noise: error "
              << std::fixed << std::setprecision(2)
              << std::abs(centsFromHz(reading.frequency, 146.832))
              << " cents, clarity " << reading.clarity << "\n";
    EXPECT_TRUE(std::abs(centsFromHz(reading.frequency, 146.832)) < 5.0);

    // Pure noise must not be reported as a pitch.
    InstrumentTuner noiseOnly;
    std::vector<float> pureNoise(InstrumentTuner::windowFrames * 2U, 0.0F);
    for (float& sample : pureNoise) {
        sample = noise() * 0.3F;
    }
    EXPECT_TRUE(!readSignal(noiseOnly, pureNoise).detected);
}

JAMLINK_TEST(tuner_write_allocates_nothing_after_construction) {
    InstrumentTuner tuner;
    const auto signal = pluckedString(110.0, InstrumentTuner::windowFrames * 2U);
    static_cast<void>(readSignal(tuner, signal));

    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_relaxed);
    constexpr std::size_t block = 128U;
    for (std::size_t round = 0U; round < 40U; ++round) {
        for (std::size_t offset = 0U; offset + block <= signal.size(); offset += block) {
            tuner.write(std::span<const float>(signal.data() + offset, block), sampleRate);
        }
        static_cast<void>(tuner.analyse());
    }
    allocationTrackingEnabled.store(false, std::memory_order_relaxed);
    // analyse() runs on a control thread, but write() is on the audio callback
    // and neither is allowed to allocate.
    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_relaxed) == 0U);
}

JAMLINK_TEST(tuner_names_notes_and_octaves) {
    EXPECT_TRUE(InstrumentTuner::noteName(69) == "A");
    EXPECT_TRUE(InstrumentTuner::noteOctave(69) == 4);
    EXPECT_TRUE(InstrumentTuner::noteName(60) == "C");
    EXPECT_TRUE(InstrumentTuner::noteOctave(60) == 4);
    EXPECT_TRUE(InstrumentTuner::noteName(40) == "E");
    EXPECT_TRUE(InstrumentTuner::noteOctave(40) == 2);
    EXPECT_TRUE(InstrumentTuner::noteName(61) == "C#");
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
              << " tuner tests passed\n";
    return failures == 0U ? 0 : 1;
}
