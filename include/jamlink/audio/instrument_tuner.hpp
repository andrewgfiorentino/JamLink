// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/realtime_atomic.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace jamlink::audio {

struct TunerReading final {
    bool detected{false};
    // Estimated fundamental in Hz.
    double frequency{0.0};
    // MIDI note number of the nearest chromatic pitch, 69 being A4.
    int midiNote{0};
    // Signed distance to that pitch, negative meaning flat.
    double cents{0.0};
    // Peak amplitude of the analysed window, for a level or confidence display.
    float level{0.0F};
    // Normalised clarity of the periodicity estimate in [0, 1].
    float clarity{0.0F};
};

// Chromatic pitch detector for a single instrument input.
//
// The tuner is a tap: write() is called from the audio callback and only copies
// into a preallocated ring, so the monitored signal path is untouched and no
// latency is added to what the musician hears. analyse() does the work and is
// called from a control thread, typically a UI timer.
//
// Detection uses the McLeod normalised square difference function, which is
// robust on plucked strings where the first harmonic often exceeds the
// fundamental. A decimated search locates the period and a full-rate pass
// refines it, so cost does not scale with the low-frequency search range.
class InstrumentTuner final {
public:
    static constexpr std::size_t windowFrames = 4'096U;

    InstrumentTuner();

    InstrumentTuner(const InstrumentTuner&) = delete;
    InstrumentTuner& operator=(const InstrumentTuner&) = delete;

    // Audio-callback side. Allocation-free and lock-free.
    void write(std::span<const float> monoSamples, std::uint32_t sampleRate) noexcept;

    // Control-thread side. Returns the reading for the most recent window.
    [[nodiscard]] TunerReading analyse() noexcept;

    // Control thread only, with the producer stopped.
    void reset() noexcept;

    // Concert pitch. Guitar tuning presets are a UI concern; the detector is
    // chromatic and only needs the reference.
    void setReferenceHz(double referenceHz) noexcept;
    [[nodiscard]] double referenceHz() const noexcept;

    // Note name for a MIDI note number, for example "A" or "C#".
    [[nodiscard]] static std::string_view noteName(int midiNote) noexcept;
    // Scientific pitch octave, so MIDI 69 reports 4.
    [[nodiscard]] static int noteOctave(int midiNote) noexcept;

private:
    [[nodiscard]] std::size_t collectWindow() noexcept;
    [[nodiscard]] double refinePeriod(
        std::size_t coarseLag,
        std::size_t frames,
        double& clarity) noexcept;

    static constexpr std::size_t ringFrames = 8'192U;
    static constexpr std::size_t ringMask = ringFrames - 1U;
    static constexpr std::size_t decimation = 4U;

    std::vector<float> ring_;
    std::vector<float> window_;
    std::vector<float> decimated_;
    std::atomic<std::uint64_t> writeCursor_{0U};
    std::atomic<std::uint32_t> sampleRate_{0U};
    RealtimeAtomicFloat referenceHz_{440.0F};
};

} // namespace jamlink::audio
