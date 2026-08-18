// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

namespace jamlink::audio {

// A click, and the bars of it before a take that stop a recording beginning
// halfway through a thought.
//
// Sample-accurate on purpose. A metronome driven from a timer would drift
// against the audio it is played over, and the drift is exactly the thing a
// musician is listening for. Everything here is counted in frames on the same
// timeline as the audio, so a click lands on a sample rather than near one.
//
// Nothing is allocated after prepare(): this runs in the audio callback.
class Metronome final {
public:
    struct Settings final {
        std::uint32_t beatsPerMinute{100U};
        // Beats in a bar. The first of each bar is accented, which is what
        // makes a count-in countable rather than just loud.
        std::uint8_t beatsPerBar{4U};
        // Bars of clicks before the take starts. Zero starts immediately.
        std::uint8_t countInBars{1U};
        float level{0.35F};
    };

    void prepare(std::uint32_t sampleRate, const Settings& settings) noexcept;

    // Tempo, changed while playing.
    //
    // Control thread. The new tempo takes effect at the next beat rather than
    // at the next sample: a change applied mid-click would cut the click in
    // half, and a change applied mid-beat would make that one beat the wrong
    // length, which is heard as a stumble at exactly the moment somebody is
    // trying to find the right tempo by ear.
    void setBeatsPerMinute(std::uint32_t beatsPerMinute) noexcept;
    [[nodiscard]] std::uint32_t beatsPerMinute() const noexcept;

    // Starts at the next call to mix(). A count-in, if configured, runs first.
    void start() noexcept;
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept { return running_; }

    // True while the count-in is still playing. A take must not begin during
    // it, or the first bar of the recording is a click track.
    [[nodiscard]] bool countingIn() const noexcept;
    // Beats left in the count-in, so the interface can show 4, 3, 2, 1 rather
    // than a spinner.
    [[nodiscard]] std::uint32_t countInBeatsRemaining() const noexcept;

    // Adds the click into `destination` in place. Realtime path.
    void mix(std::span<float> destination) noexcept;

    // Beats since start, count-in included. For a display that has to agree
    // with what is being heard rather than with a separate timer.
    [[nodiscard]] std::uint64_t beatsElapsed() const noexcept { return beatsElapsed_; }

    void reset() noexcept;

private:
    [[nodiscard]] float clickSample(std::size_t frameInClick, bool accented) const noexcept;

    [[nodiscard]] static std::uint32_t clampTempo(std::uint32_t beatsPerMinute) noexcept;
    [[nodiscard]] std::size_t framesForTempo(std::uint32_t beatsPerMinute) const noexcept;

    std::uint32_t sampleRate_{48'000U};
    // Written by whoever changes the tempo, read by the audio callback at
    // a beat boundary. One word, so no lock and nothing for the callback
    // to wait on.
    std::atomic<std::uint32_t> requestedBeatsPerMinute_{0U};
    Settings settings_{};
    std::size_t framesPerBeat_{0U};
    std::size_t clickFrames_{0U};
    std::size_t frameInBeat_{0U};
    std::uint64_t beatsElapsed_{0U};
    std::uint32_t countInBeats_{0U};
    bool running_{false};
};

} // namespace jamlink::audio
