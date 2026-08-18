// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/metronome.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace jamlink::audio {
namespace {

// Short enough to be a click rather than a note, long enough to have a pitch a
// player can hear over an instrument.
constexpr double clickSeconds = 0.012;
constexpr double accentedHertz = 1'760.0;
constexpr double ordinaryHertz = 880.0;

} // namespace

std::uint32_t Metronome::clampTempo(std::uint32_t beatsPerMinute) noexcept {
    return std::clamp<std::uint32_t>(beatsPerMinute, 30U, 300U);
}

std::size_t Metronome::framesForTempo(std::uint32_t beatsPerMinute) const noexcept {
    // Rounded to whole frames once rather than accumulated per beat: a
    // fractional remainder added up over a few hundred beats is audible drift
    // against the audio it is played over.
    return static_cast<std::size_t>(
        (static_cast<double>(sampleRate_) * 60.0)
        / static_cast<double>(clampTempo(beatsPerMinute)) + 0.5);
}

void Metronome::prepare(std::uint32_t sampleRate, const Settings& settings) noexcept {
    sampleRate_ = sampleRate == 0U ? 48'000U : sampleRate;
    settings_ = settings;
    settings_.beatsPerMinute = clampTempo(settings_.beatsPerMinute);
    settings_.beatsPerBar = std::clamp<std::uint8_t>(settings_.beatsPerBar, 1U, 16U);
    settings_.level = std::clamp(settings_.level, 0.0F, 1.0F);
    framesPerBeat_ = framesForTempo(settings_.beatsPerMinute);
    clickFrames_ = std::min(
        framesPerBeat_,
        static_cast<std::size_t>(static_cast<double>(sampleRate_) * clickSeconds));
    requestedBeatsPerMinute_.store(0U, std::memory_order_relaxed);
    reset();
}

void Metronome::setBeatsPerMinute(std::uint32_t beatsPerMinute) noexcept {
    requestedBeatsPerMinute_.store(clampTempo(beatsPerMinute), std::memory_order_release);
}

std::uint32_t Metronome::beatsPerMinute() const noexcept {
    const std::uint32_t pending = requestedBeatsPerMinute_.load(std::memory_order_acquire);
    // What will be playing, not what is playing for the next few milliseconds.
    // A tempo box that snapped back to the old number until the next beat
    // would look broken.
    return pending == 0U ? settings_.beatsPerMinute : pending;
}

void Metronome::reset() noexcept {
    frameInBeat_ = 0U;
    beatsElapsed_ = 0U;
    countInBeats_ = 0U;
    running_ = false;
}

void Metronome::start() noexcept {
    frameInBeat_ = 0U;
    beatsElapsed_ = 0U;
    countInBeats_ = static_cast<std::uint32_t>(settings_.countInBars)
        * static_cast<std::uint32_t>(settings_.beatsPerBar);
    running_ = true;
}

void Metronome::stop() noexcept {
    running_ = false;
    countInBeats_ = 0U;
}

bool Metronome::countingIn() const noexcept {
    return running_ && beatsElapsed_ < countInBeats_;
}

std::uint32_t Metronome::countInBeatsRemaining() const noexcept {
    if (!countingIn()) {
        return 0U;
    }
    return countInBeats_ - static_cast<std::uint32_t>(beatsElapsed_);
}

float Metronome::clickSample(std::size_t frameInClick, bool accented) const noexcept {
    const double position = static_cast<double>(frameInClick);
    const double length = static_cast<double>(clickFrames_);
    // A raised-cosine envelope. A square-edged click would produce a broadband
    // transient that the encoder then spends its bits on.
    const double envelope = 0.5 * (1.0 - std::cos(2.0 * std::numbers::pi * position / length));
    const double hertz = accented ? accentedHertz : ordinaryHertz;
    const double phase = 2.0 * std::numbers::pi * hertz * position
        / static_cast<double>(sampleRate_);
    return static_cast<float>(
        envelope * std::sin(phase) * static_cast<double>(settings_.level));
}

void Metronome::mix(std::span<float> destination) noexcept {
    if (!running_ || framesPerBeat_ == 0U) {
        return;
    }
    for (float& sample : destination) {
        if (frameInBeat_ < clickFrames_) {
            const bool accented =
                (beatsElapsed_ % static_cast<std::uint64_t>(settings_.beatsPerBar)) == 0U;
            // Added rather than replacing, so the click sits over the playing
            // instead of punching a hole in it.
            sample += clickSample(frameInBeat_, accented);
        }
        ++frameInBeat_;
        if (frameInBeat_ >= framesPerBeat_) {
            frameInBeat_ = 0U;
            ++beatsElapsed_;
            // A beat boundary is the only safe moment to change tempo, and the
            // only musical one.
            const std::uint32_t requested =
                requestedBeatsPerMinute_.exchange(0U, std::memory_order_acquire);
            if (requested != 0U && requested != settings_.beatsPerMinute) {
                settings_.beatsPerMinute = requested;
                framesPerBeat_ = framesForTempo(requested);
                // The click keeps its length until the beat becomes shorter
                // than the click is, so a fast tempo does not turn into a drone.
                clickFrames_ = std::min(
                    framesPerBeat_,
                    static_cast<std::size_t>(
                        static_cast<double>(sampleRate_) * clickSeconds));
            }
        }
    }
}

} // namespace jamlink::audio
