// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/audio/spsc_audio_ring.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace jamlink::record {

// Isolated tracks, because a take is worth far more when the parts are
// separable afterwards. The user still only ever presses one button.
enum class RecordTrack : std::uint8_t {
    // The live take: four tracks sharing one timeline, so they line up with
    // each other and with what the musicians actually heard.
    LocalInstrument = 0U,
    LocalVoice = 1U,
    RemoteInstrument = 2U,
    RemoteVoice = 3U,
    // Local originals, taken in the capture callback at the capture device's
    // own rate, before the monitor converter and before anything the network
    // could do to them.
    //
    // The live tracks above are what was heard; these are what was played. A
    // dropout damages the first and cannot touch the second, which is what
    // makes a take repairable after the fact.
    LocalInstrumentOriginal = 4U,
    LocalVoiceOriginal = 5U
};

inline constexpr std::size_t recordTrackCount = 6U;

// Which tracks a take is allowed to contain.
//
// A musician who does not want talkback in the take must be able to leave it
// out, and "record everything and delete it afterwards" is not the same
// promise: the audio was still written to disk.
//
// An excluded track gets no file at all, rather than an empty one. An empty
// WAV is exactly what a track that failed leaves behind, and a take that
// cannot tell a deliberate omission from a fault is a take that lies about
// itself.
struct RecordTrackSelection final {
    std::array<bool, recordTrackCount> included{
        true, true, true, true, true, true};

    [[nodiscard]] bool includes(RecordTrack track) const noexcept {
        return included[static_cast<std::size_t>(track)];
    }
    void exclude(RecordTrack track) noexcept {
        included[static_cast<std::size_t>(track)] = false;
    }
    [[nodiscard]] bool any() const noexcept {
        for (const bool value : included) {
            if (value) {
                return true;
            }
        }
        return false;
    }
};

[[nodiscard]] constexpr bool isLocalOriginal(RecordTrack track) noexcept {
    return track == RecordTrack::LocalInstrumentOriginal
        || track == RecordTrack::LocalVoiceOriginal;
}

[[nodiscard]] constexpr std::size_t trackIndex(RecordTrack track) noexcept {
    return static_cast<std::size_t>(track);
}

struct RecorderTelemetry final {
    bool recording{false};
    bool failed{false};
    std::uint64_t framesWritten{0U};
    // Frames the audio callback could not hand over because the disk worker
    // fell behind. Any value above zero means the take has a gap.
    std::uint64_t droppedFrames{0U};
    std::uint64_t bytesWritten{0U};
    std::uint32_t elapsedSeconds{0U};
};

// Multitrack recorder.
//
// write() is called from the audio callback and only touches preallocated
// lock-free rings: no allocation, no locking, and above all no filesystem
// work. A dedicated worker drains the rings and writes 32-bit float WAV files,
// one per track.
//
// Every track advances by the same number of frames on every callback, with
// silence where a source has nothing, so the files stay sample-aligned and can
// be dropped onto one timeline without hunting for an offset.
class SessionRecorder final {
public:
    explicit SessionRecorder(std::size_t ringFrames = 1U << 18U);
    ~SessionRecorder();

    SessionRecorder(const SessionRecorder&) = delete;
    SessionRecorder& operator=(const SessionRecorder&) = delete;

    // Control thread. Creates directory/name and opens one file per track.
    [[nodiscard]] bool start(
        const std::filesystem::path& directory,
        const std::string& sessionName,
        std::uint32_t sampleRate);

    // The local originals keep their capture device's own rate rather than
    // being resampled to the timeline's, because a resampled original is no
    // longer an original. Zero means "the same as the timeline", which is the
    // common case where everything already runs at 48 kHz.
    [[nodiscard]] bool start(
        const std::filesystem::path& directory,
        const std::string& sessionName,
        std::uint32_t sampleRate,
        std::uint32_t localInstrumentRate,
        std::uint32_t localVoiceRate);

    // As above, but only the selected tracks are opened and written. Refuses a
    // selection that excludes everything: a take of nothing is a mistake, not
    // a request.
    [[nodiscard]] bool start(
        const std::filesystem::path& directory,
        const std::string& sessionName,
        std::uint32_t sampleRate,
        std::uint32_t localInstrumentRate,
        std::uint32_t localVoiceRate,
        const RecordTrackSelection& selection);

    // What the running take is actually capturing. Control thread.
    [[nodiscard]] RecordTrackSelection selection() const noexcept { return selection_; }

    // Control thread. Drains what is queued and finalises every header.
    void stop() noexcept;

    [[nodiscard]] bool recording() const noexcept {
        return recording_.load(std::memory_order_acquire);
    }

    [[nodiscard]] RecorderTelemetry telemetry() const noexcept;
    [[nodiscard]] std::filesystem::path sessionDirectory() const;

    // Audio-callback side.
    void write(RecordTrack track, std::span<const float> monoSamples) noexcept;

    [[nodiscard]] static std::string trackFileName(RecordTrack track);

private:
    struct Track final {
        std::unique_ptr<audio::SpscAudioRing> ring;
        std::ofstream file;
        std::uint64_t frames{0U};
        std::uint32_t sampleRate{0U};
    };

    void run() noexcept;
    // Rewrites the RIFF, fact, and data sizes so a file left behind by a crash
    // is still playable up to the last update.
    void refreshSizes(Track& track) noexcept;
    void closeTracks() noexcept;

    const std::size_t ringFrames_;
    std::array<Track, recordTrackCount> tracks_;
    RecordTrackSelection selection_;
    std::vector<float> scratch_;
    std::filesystem::path sessionDirectory_;
    std::uint32_t sampleRate_{48'000U};
    std::thread worker_;
    std::atomic<bool> recording_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> failed_{false};
    std::atomic<std::uint64_t> framesWritten_{0U};
    std::atomic<std::uint64_t> droppedFrames_{0U};
    std::atomic<std::uint64_t> bytesWritten_{0U};
};

} // namespace jamlink::record
