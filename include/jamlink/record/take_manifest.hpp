// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace jamlink::record {

// What a recording actually is, as opposed to a directory of WAV files.
//
// A playable WAV proves that samples reached a disk. It does not prove that the
// take finished, that both musicians' audio is present, that nothing was
// dropped, or that what survived is what was played. A take that was
// interrupted by a crash leaves files that open perfectly and are missing the
// last ten seconds, and nothing on disk says so.
//
// The manifest is where that is said. It is written outside the realtime path,
// replaced atomically, and only ever reaches Ready after a finalisation that
// actually succeeded.
enum class TakeState : std::uint8_t {
    Preparing,
    Recording,
    Stopping,
    Finalizing,
    // Finalised, with every source accounted for.
    Ready,
    // Found unfinished at startup. Whatever survived is kept and the gaps are
    // named; it is never quietly promoted to Ready.
    RecoveredNeedsReview,
    Failed,
};

[[nodiscard]] std::string_view takeStateName(TakeState state) noexcept;
[[nodiscard]] std::optional<TakeState> takeStateFromName(std::string_view name) noexcept;

// One logical source within a take.
//
// Identity is deliberately richer than a filename, because the same logical
// source appears in several forms over a take's life: captured locally,
// received over the network, and later possibly replaced by the other side's
// pristine original. All three are the same musician playing the same
// instrument, and a take has to be able to say so.
struct TakeSource final {
    std::string sourceId;
    // Whose audio this is. Empty for a take recorded alone.
    std::string participantId;
    // "instrument" or "voice".
    std::string role;
    // "local-capture" for audio taken before the network could damage it, or
    // "network-received" for what actually arrived.
    std::string origin;
    std::string fileName;
    std::uint64_t frames{0U};
    // Frames known to be missing: dropped by the writer, or concealed on the
    // way in. A take with gaps is still worth keeping, but never worth
    // presenting as clean.
    std::uint64_t knownGapFrames{0U};
    // Empty until finalisation. A source is not trusted until it verifies.
    std::string sha256;
};

struct TakeManifest final {
    // Bumped when the format changes in a way an older reader cannot handle.
    std::uint32_t manifestVersion{1U};
    std::string takeId;
    std::string sessionId;
    std::uint64_t startedAtMillisecond{0U};
    std::uint64_t endedAtMillisecond{0U};
    std::uint32_t sampleRate{0U};
    std::string applicationVersion;
    // The wire format the received sources arrived in, which decides what their
    // defects can be attributed to.
    std::string mediaFormat;
    TakeState state{TakeState::Preparing};
    std::vector<TakeSource> sources;
    // Semantic events only: "peer dropped", "device lost". No secrets, no
    // endpoints, no chat.
    std::vector<std::string> interruptions;
    std::uint64_t droppedFrames{0U};

    [[nodiscard]] bool complete() const noexcept { return state == TakeState::Ready; }
    [[nodiscard]] bool hasKnownGaps() const noexcept;
};

// A deliberately plain line format rather than JSON: it is versioned, it is
// readable by a musician who opens it, and a tolerant parser for it is small
// enough to be obviously correct. A recording's own record of itself is a bad
// place for a subtle parser bug.
[[nodiscard]] std::string serialiseTake(const TakeManifest& manifest);
[[nodiscard]] std::optional<TakeManifest> parseTake(std::string_view text);

// Crash-safe persistence for a take in progress.
//
// Every write replaces the manifest atomically: the new text is written beside
// it and renamed over the top, so a process killed mid-write leaves either the
// previous manifest or the new one, never a half-written file. None of this is
// ever called from the audio callback.
class TakeJournal final {
public:
    static constexpr const char* manifestFileName = "take.jamlink";

    // Records that a take has begun. From here until finalise() succeeds, the
    // manifest on disk says the take was in progress, which is what makes an
    // interruption detectable at all.
    [[nodiscard]] static bool begin(
        const std::filesystem::path& takeDirectory, const TakeManifest& manifest);

    // Periodic progress. Cheap enough to call while recording, from the writer
    // thread.
    [[nodiscard]] static bool checkpoint(
        const std::filesystem::path& takeDirectory, const TakeManifest& manifest);

    // Only this promotes a take to Ready, and only if the write succeeds.
    [[nodiscard]] static bool finalise(
        const std::filesystem::path& takeDirectory, TakeManifest manifest);

    [[nodiscard]] static std::optional<TakeManifest> read(
        const std::filesystem::path& takeDirectory);

    // Turns an interrupted take into an honest one: keeps every file, marks it
    // as needing review, and records why. Never deletes anything.
    [[nodiscard]] static std::optional<TakeManifest> recover(
        const std::filesystem::path& takeDirectory);

    // Takes under `root` that were left mid-recording. Oldest first.
    [[nodiscard]] static std::vector<std::filesystem::path> findInterrupted(
        const std::filesystem::path& root);
};

} // namespace jamlink::record
