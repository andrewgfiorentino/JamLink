// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/instrument_tuner.hpp"

#include <algorithm>
#include <array>
#include <cmath>

namespace jamlink::audio {
namespace {

// A five string bass low B is about 31 Hz and the top of a 24 fret guitar is
// about 1319 Hz. The search covers both with margin.
constexpr double lowestHz = 28.0;
constexpr double highestHz = 1'600.0;
// Below this the reading is reported as undetected rather than guessed at.
constexpr float minimumLevel = 0.004F;
constexpr float minimumClarity = 0.6F;
// McLeod picks the first peak reaching this fraction of the highest peak,
// which is what keeps a strong first harmonic from being read as the pitch.
constexpr double peakAcceptance = 0.9;

constexpr std::array<std::string_view, 12U> noteNames{
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// Normalised square difference at one lag: the McLeod formulation, where the
// denominator is the summed energy of both compared segments.
[[nodiscard]] double normalisedSquareDifference(
    const float* samples,
    std::size_t frames,
    std::size_t lag) noexcept {
    double correlation = 0.0;
    double energy = 0.0;
    const std::size_t count = frames - lag;
    for (std::size_t index = 0U; index < count; ++index) {
        const double current = samples[index];
        const double delayed = samples[index + lag];
        correlation += current * delayed;
        energy += current * current + delayed * delayed;
    }
    return energy > 0.0 ? (2.0 * correlation) / energy : 0.0;
}

// Parabolic interpolation through three points, returning the offset of the
// vertex from the centre point. This is what lifts the estimate from whole
// samples to a fraction of one.
[[nodiscard]] double parabolicOffset(double left, double centre, double right) noexcept {
    const double denominator = 2.0 * (2.0 * centre - left - right);
    if (std::abs(denominator) < 1.0e-12) {
        return 0.0;
    }
    return std::clamp((right - left) / denominator, -1.0, 1.0);
}

} // namespace

InstrumentTuner::InstrumentTuner()
    : ring_(ringFrames, 0.0F),
      window_(windowFrames, 0.0F),
      decimated_(windowFrames / decimation, 0.0F) {}

void InstrumentTuner::write(
    std::span<const float> monoSamples,
    std::uint32_t sampleRate) noexcept {
    if (sampleRate < 8'000U || sampleRate > 384'000U) {
        return;
    }
    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    std::uint64_t cursor = writeCursor_.load(std::memory_order_relaxed);
    for (const float sample : monoSamples) {
        ring_[static_cast<std::size_t>(cursor) & ringMask] =
            std::isfinite(sample) ? sample : 0.0F;
        ++cursor;
    }
    writeCursor_.store(cursor, std::memory_order_release);
}

std::size_t InstrumentTuner::collectWindow() noexcept {
    const std::uint64_t cursor = writeCursor_.load(std::memory_order_acquire);
    if (cursor < windowFrames) {
        return 0U;
    }
    const std::uint64_t start = cursor - windowFrames;
    for (std::size_t index = 0U; index < windowFrames; ++index) {
        window_[index] = ring_[static_cast<std::size_t>(start + index) & ringMask];
    }
    return windowFrames;
}

double InstrumentTuner::refinePeriod(
    std::size_t coarseLag,
    std::size_t frames,
    double& clarity) noexcept {
    const std::size_t minimum = 2U;
    const std::size_t maximum = frames / 2U;
    const std::size_t low = std::max(minimum, coarseLag > 4U ? coarseLag - 4U : minimum);
    const std::size_t high = std::min(maximum - 1U, coarseLag + 4U);

    double bestValue = -1.0;
    std::size_t bestLag = 0U;
    for (std::size_t lag = low; lag <= high; ++lag) {
        const double value = normalisedSquareDifference(window_.data(), frames, lag);
        if (value > bestValue) {
            bestValue = value;
            bestLag = lag;
        }
    }
    if (bestLag <= minimum || bestLag + 1U >= maximum) {
        clarity = 0.0;
        return 0.0;
    }

    const double left = normalisedSquareDifference(window_.data(), frames, bestLag - 1U);
    const double right = normalisedSquareDifference(window_.data(), frames, bestLag + 1U);
    clarity = std::clamp(bestValue, 0.0, 1.0);
    return static_cast<double>(bestLag) + parabolicOffset(left, bestValue, right);
}

TunerReading InstrumentTuner::analyse() noexcept {
    TunerReading reading;
    const std::uint32_t sampleRate = sampleRate_.load(std::memory_order_relaxed);
    if (sampleRate == 0U || collectWindow() == 0U) {
        return reading;
    }

    float peak = 0.0F;
    for (const float sample : window_) {
        peak = std::max(peak, std::abs(sample));
    }
    reading.level = std::min(peak, 1.0F);
    if (peak < minimumLevel) {
        return reading;
    }

    // Remove any DC offset, which otherwise dominates the low-lag correlation
    // on interfaces that pass a small bias.
    double mean = 0.0;
    for (const float sample : window_) {
        mean += sample;
    }
    mean /= static_cast<double>(window_.size());
    for (float& sample : window_) {
        sample -= static_cast<float>(mean);
    }

    // Box-average decimation. The anti-alias requirement is mild here because
    // the coarse pass only has to land within a few samples of the true period.
    const std::size_t coarseFrames = window_.size() / decimation;
    for (std::size_t index = 0U; index < coarseFrames; ++index) {
        double sum = 0.0;
        for (std::size_t tap = 0U; tap < decimation; ++tap) {
            sum += window_[index * decimation + tap];
        }
        decimated_[index] = static_cast<float>(sum / static_cast<double>(decimation));
    }

    const double coarseRate = static_cast<double>(sampleRate) / static_cast<double>(decimation);
    const auto coarseMinimum = std::max<std::size_t>(
        2U, static_cast<std::size_t>(std::floor(coarseRate / highestHz)));
    const auto coarseMaximum = std::min<std::size_t>(
        coarseFrames / 2U - 1U, static_cast<std::size_t>(std::ceil(coarseRate / lowestHz)));
    if (coarseMinimum >= coarseMaximum) {
        return reading;
    }

    // McLeod peak picking: collect local maxima, then take the first one that
    // reaches peakAcceptance of the largest. Taking the largest outright would
    // often select a multiple of the true period.
    double highestPeak = 0.0;
    std::size_t firstAcceptedLag = 0U;
    double previous = normalisedSquareDifference(
        decimated_.data(), coarseFrames, coarseMinimum);
    double current = normalisedSquareDifference(
        decimated_.data(), coarseFrames, coarseMinimum + 1U);

    // Two passes over the same cheap function: one to find the tallest peak,
    // one to take the first peak that clears the acceptance threshold.
    for (std::size_t lag = coarseMinimum + 1U; lag + 1U <= coarseMaximum; ++lag) {
        const double next = normalisedSquareDifference(
            decimated_.data(), coarseFrames, lag + 1U);
        if (current > previous && current >= next) {
            highestPeak = std::max(highestPeak, current);
        }
        previous = current;
        current = next;
    }
    if (highestPeak <= 0.0) {
        return reading;
    }

    previous = normalisedSquareDifference(decimated_.data(), coarseFrames, coarseMinimum);
    current = normalisedSquareDifference(decimated_.data(), coarseFrames, coarseMinimum + 1U);
    for (std::size_t lag = coarseMinimum + 1U; lag + 1U <= coarseMaximum; ++lag) {
        const double next = normalisedSquareDifference(
            decimated_.data(), coarseFrames, lag + 1U);
        if (current > previous && current >= next && current >= highestPeak * peakAcceptance) {
            firstAcceptedLag = lag;
            break;
        }
        previous = current;
        current = next;
    }
    if (firstAcceptedLag == 0U) {
        return reading;
    }

    double clarity = 0.0;
    const double period = refinePeriod(
        firstAcceptedLag * decimation, window_.size(), clarity);
    if (period <= 0.0) {
        return reading;
    }

    const double frequency = static_cast<double>(sampleRate) / period;
    if (frequency < lowestHz || frequency > highestHz) {
        return reading;
    }

    reading.clarity = static_cast<float>(clarity);
    if (clarity < static_cast<double>(minimumClarity)) {
        return reading;
    }

    const double reference = static_cast<double>(referenceHz_.load());
    const double midi = 69.0 + 12.0 * std::log2(frequency / reference);
    const double nearest = std::round(midi);
    reading.detected = true;
    reading.frequency = frequency;
    reading.midiNote = static_cast<int>(nearest);
    reading.cents = (midi - nearest) * 100.0;
    return reading;
}

void InstrumentTuner::reset() noexcept {
    std::fill(ring_.begin(), ring_.end(), 0.0F);
    std::fill(window_.begin(), window_.end(), 0.0F);
    std::fill(decimated_.begin(), decimated_.end(), 0.0F);
    writeCursor_.store(0U, std::memory_order_relaxed);
    sampleRate_.store(0U, std::memory_order_relaxed);
}

void InstrumentTuner::setReferenceHz(double referenceHz) noexcept {
    referenceHz_.store(static_cast<float>(std::clamp(referenceHz, 380.0, 500.0)));
}

double InstrumentTuner::referenceHz() const noexcept {
    return static_cast<double>(referenceHz_.load());
}

std::string_view InstrumentTuner::noteName(int midiNote) noexcept {
    if (midiNote < 0) {
        return "-";
    }
    return noteNames[static_cast<std::size_t>(midiNote % 12)];
}

int InstrumentTuner::noteOctave(int midiNote) noexcept {
    return midiNote / 12 - 1;
}

} // namespace jamlink::audio
