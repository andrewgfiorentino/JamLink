// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/record/session_recorder.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <system_error>

namespace jamlink::record {
namespace {

// 32-bit float WAV. Lossless, no conversion on the way to disk, and the right
// starting point for the local-master workflow.
constexpr std::uint16_t formatIeeeFloat = 3U;
constexpr std::uint16_t bitsPerSample = 32U;
constexpr std::uint16_t channelCount = 1U;
constexpr std::size_t headerBytes = 58U;
constexpr std::size_t riffSizeOffset = 4U;
constexpr std::size_t factSampleOffset = 46U;
constexpr std::size_t dataSizeOffset = 54U;
// How often the on-disk sizes are brought up to date while recording.
constexpr auto sizeRefreshInterval = std::chrono::seconds(2);
constexpr auto drainInterval = std::chrono::milliseconds(40);

void writeU16(std::array<std::uint8_t, headerBytes>& header,
              std::size_t offset,
              std::uint16_t value) noexcept {
    header[offset] = static_cast<std::uint8_t>(value & 0xFFU);
    header[offset + 1U] = static_cast<std::uint8_t>((value >> 8U) & 0xFFU);
}

void writeU32(std::array<std::uint8_t, headerBytes>& header,
              std::size_t offset,
              std::uint32_t value) noexcept {
    for (std::size_t index = 0U; index < 4U; ++index) {
        header[offset + index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
}

void writeTag(std::array<std::uint8_t, headerBytes>& header,
              std::size_t offset,
              const char (&tag)[5]) noexcept {
    for (std::size_t index = 0U; index < 4U; ++index) {
        header[offset + index] = static_cast<std::uint8_t>(tag[index]);
    }
}

[[nodiscard]] std::array<std::uint8_t, headerBytes> makeHeader(std::uint32_t sampleRate) noexcept {
    std::array<std::uint8_t, headerBytes> header{};
    const std::uint32_t byteRate = sampleRate * channelCount * (bitsPerSample / 8U);
    const std::uint16_t blockAlign = channelCount * (bitsPerSample / 8U);

    writeTag(header, 0U, "RIFF");
    writeU32(header, 4U, static_cast<std::uint32_t>(headerBytes - 8U));
    writeTag(header, 8U, "WAVE");
    writeTag(header, 12U, "fmt ");
    // Non-PCM formats carry an 18 byte fmt chunk and a fact chunk.
    writeU32(header, 16U, 18U);
    writeU16(header, 20U, formatIeeeFloat);
    writeU16(header, 22U, channelCount);
    writeU32(header, 24U, sampleRate);
    writeU32(header, 28U, byteRate);
    writeU16(header, 32U, blockAlign);
    writeU16(header, 34U, bitsPerSample);
    writeU16(header, 36U, 0U);
    writeTag(header, 38U, "fact");
    writeU32(header, 42U, 4U);
    writeU32(header, 46U, 0U);
    writeTag(header, 50U, "data");
    writeU32(header, 54U, 0U);
    return header;
}

void patchU32(std::ofstream& file, std::size_t offset, std::uint32_t value) noexcept {
    std::array<std::uint8_t, 4U> bytes{};
    for (std::size_t index = 0U; index < 4U; ++index) {
        bytes[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
    }
    const auto resume = file.tellp();
    file.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    file.seekp(resume, std::ios::beg);
}

} // namespace

SessionRecorder::SessionRecorder(std::size_t ringFrames)
    : ringFrames_(ringFrames), scratch_(8'192U, 0.0F) {
    for (Track& track : tracks_) {
        track.ring = std::make_unique<audio::SpscAudioRing>(ringFrames_, 1U);
    }
}

SessionRecorder::~SessionRecorder() { stop(); }

std::string SessionRecorder::trackFileName(RecordTrack track) {
    switch (track) {
    case RecordTrack::LocalInstrument:
        return "instrument.wav";
    case RecordTrack::LocalVoice:
        return "voice.wav";
    case RecordTrack::RemoteInstrument:
        return "friend-instrument.wav";
    case RecordTrack::RemoteVoice:
        return "friend-voice.wav";
    }
    return "track.wav";
}

bool SessionRecorder::start(
    const std::filesystem::path& directory,
    const std::string& sessionName,
    std::uint32_t sampleRate) {
    if (recording_.load(std::memory_order_acquire)) {
        return false;
    }
    if (sampleRate < 8'000U || sampleRate > 384'000U || sessionName.empty()) {
        return false;
    }

    std::error_code error;
    const std::filesystem::path session = directory / sessionName;
    std::filesystem::create_directories(session, error);
    if (error) {
        failed_.store(true, std::memory_order_release);
        return false;
    }

    sampleRate_ = sampleRate;
    sessionDirectory_ = session;
    const auto header = makeHeader(sampleRate);
    for (std::size_t index = 0U; index < recordTrackCount; ++index) {
        Track& track = tracks_[index];
        track.ring->clear();
        track.frames = 0U;
        track.file.clear();
        track.file.open(
            session / trackFileName(static_cast<RecordTrack>(index)),
            std::ios::binary | std::ios::trunc);
        if (!track.file.is_open()) {
            closeTracks();
            failed_.store(true, std::memory_order_release);
            return false;
        }
        track.file.write(
            reinterpret_cast<const char*>(header.data()),
            static_cast<std::streamsize>(header.size()));
    }

    framesWritten_.store(0U, std::memory_order_relaxed);
    droppedFrames_.store(0U, std::memory_order_relaxed);
    bytesWritten_.store(0U, std::memory_order_relaxed);
    failed_.store(false, std::memory_order_relaxed);
    stopRequested_.store(false, std::memory_order_release);
    recording_.store(true, std::memory_order_release);
    worker_ = std::thread([this] { run(); });
    return true;
}

void SessionRecorder::stop() noexcept {
    if (!recording_.load(std::memory_order_acquire)) {
        return;
    }
    // Clearing this first stops the audio callback handing over more frames,
    // so the final drain below is guaranteed to reach the end of the take.
    recording_.store(false, std::memory_order_release);
    stopRequested_.store(true, std::memory_order_release);
    if (worker_.joinable()) {
        worker_.join();
    }
    closeTracks();
}

void SessionRecorder::write(RecordTrack track, std::span<const float> monoSamples) noexcept {
    if (!recording_.load(std::memory_order_acquire) || monoSamples.empty()) {
        return;
    }
    const std::size_t index = trackIndex(track);
    if (index >= recordTrackCount) {
        return;
    }
    if (tracks_[index].ring->write(monoSamples) != monoSamples.size()) {
        droppedFrames_.fetch_add(monoSamples.size(), std::memory_order_relaxed);
    }
}

void SessionRecorder::run() noexcept {
    auto lastRefresh = std::chrono::steady_clock::now();
    for (;;) {
        const bool finishing = stopRequested_.load(std::memory_order_acquire);
        std::size_t drained = 0U;
        for (Track& track : tracks_) {
            for (;;) {
                const std::size_t available = track.ring->availableReadFrames();
                if (available == 0U) {
                    break;
                }
                const std::size_t frames = std::min(available, scratch_.size());
                const std::span<float> block(scratch_.data(), frames);
                static_cast<void>(track.ring->readAndZeroFill(block));
                track.file.write(
                    reinterpret_cast<const char*>(block.data()),
                    static_cast<std::streamsize>(frames * sizeof(float)));
                if (!track.file) {
                    failed_.store(true, std::memory_order_release);
                    break;
                }
                track.frames += frames;
                drained += frames;
            }
        }
        bytesWritten_.fetch_add(
            drained * sizeof(float), std::memory_order_relaxed);
        // The longest track is the take length; they only differ by whatever is
        // still queued at this instant.
        std::uint64_t longest = 0U;
        for (const Track& track : tracks_) {
            longest = std::max(longest, track.frames);
        }
        framesWritten_.store(longest, std::memory_order_relaxed);

        const auto now = std::chrono::steady_clock::now();
        if (finishing || now - lastRefresh >= sizeRefreshInterval) {
            for (Track& track : tracks_) {
                refreshSizes(track);
            }
            lastRefresh = now;
        }
        if (finishing) {
            return;
        }
        std::this_thread::sleep_for(drainInterval);
    }
}

void SessionRecorder::refreshSizes(Track& track) noexcept {
    if (!track.file.is_open()) {
        return;
    }
    const auto dataBytes = static_cast<std::uint32_t>(
        std::min<std::uint64_t>(track.frames * sizeof(float), 0xFFFFFFFFULL - headerBytes));
    patchU32(track.file, riffSizeOffset, static_cast<std::uint32_t>(headerBytes - 8U) + dataBytes);
    patchU32(track.file, factSampleOffset, static_cast<std::uint32_t>(track.frames));
    patchU32(track.file, dataSizeOffset, dataBytes);
    track.file.flush();
}

void SessionRecorder::closeTracks() noexcept {
    for (Track& track : tracks_) {
        if (track.file.is_open()) {
            refreshSizes(track);
            track.file.close();
        }
    }
}

RecorderTelemetry SessionRecorder::telemetry() const noexcept {
    RecorderTelemetry snapshot;
    snapshot.recording = recording_.load(std::memory_order_acquire);
    snapshot.failed = failed_.load(std::memory_order_acquire);
    snapshot.framesWritten = framesWritten_.load(std::memory_order_relaxed);
    snapshot.droppedFrames = droppedFrames_.load(std::memory_order_relaxed);
    snapshot.bytesWritten = bytesWritten_.load(std::memory_order_relaxed);
    snapshot.elapsedSeconds = sampleRate_ == 0U
        ? 0U
        : static_cast<std::uint32_t>(snapshot.framesWritten / sampleRate_);
    return snapshot;
}

std::filesystem::path SessionRecorder::sessionDirectory() const { return sessionDirectory_; }

} // namespace jamlink::record
