// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/record/take_manifest.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <system_error>

namespace jamlink::record {
namespace {

// Tabs separate a key from its value, so a value may contain spaces without
// needing quoting rules that a hand-written parser could get wrong.
constexpr char separator = '\t';

[[nodiscard]] std::string escapeValue(std::string_view value) {
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        // Newlines and tabs would break the line format, so they never survive
        // into a value. Nothing legitimate here contains either.
        result.push_back(
            (character == '\n' || character == '\r' || character == separator)
                ? ' ' : character);
    }
    return result;
}

void writeField(std::ostringstream& out, std::string_view key, std::string_view value) {
    out << key << separator << escapeValue(value) << '\n';
}

void writeField(std::ostringstream& out, std::string_view key, std::uint64_t value) {
    out << key << separator << value << '\n';
}

[[nodiscard]] std::uint64_t parseNumber(std::string_view value) noexcept {
    std::uint64_t result = 0U;
    for (const char character : value) {
        if (character < '0' || character > '9') {
            return result;
        }
        result = result * 10U + static_cast<std::uint64_t>(character - '0');
    }
    return result;
}

} // namespace

std::string_view takeStateName(TakeState state) noexcept {
    switch (state) {
    case TakeState::Preparing: return "Preparing";
    case TakeState::Recording: return "Recording";
    case TakeState::Stopping: return "Stopping";
    case TakeState::Finalizing: return "Finalizing";
    case TakeState::Ready: return "Ready";
    case TakeState::RecoveredNeedsReview: return "RecoveredNeedsReview";
    case TakeState::Failed: return "Failed";
    }
    return "Unknown";
}

std::optional<TakeState> takeStateFromName(std::string_view name) noexcept {
    for (const auto state : {TakeState::Preparing, TakeState::Recording, TakeState::Stopping,
                             TakeState::Finalizing, TakeState::Ready,
                             TakeState::RecoveredNeedsReview, TakeState::Failed}) {
        if (takeStateName(state) == name) {
            return state;
        }
    }
    return std::nullopt;
}

bool TakeManifest::hasKnownGaps() const noexcept {
    if (droppedFrames != 0U) {
        return true;
    }
    return std::any_of(sources.begin(), sources.end(), [](const TakeSource& source) {
        return source.knownGapFrames != 0U;
    });
}

std::string serialiseTake(const TakeManifest& manifest) {
    std::ostringstream out;
    out << "jamlink-take" << separator << manifest.manifestVersion << '\n';
    writeField(out, "takeId", manifest.takeId);
    writeField(out, "sessionId", manifest.sessionId);
    writeField(out, "startedAt", manifest.startedAtMillisecond);
    writeField(out, "endedAt", manifest.endedAtMillisecond);
    writeField(out, "sampleRate", static_cast<std::uint64_t>(manifest.sampleRate));
    writeField(out, "applicationVersion", manifest.applicationVersion);
    writeField(out, "mediaFormat", manifest.mediaFormat);
    writeField(out, "state", takeStateName(manifest.state));
    writeField(out, "droppedFrames", manifest.droppedFrames);
    for (const auto& excluded : manifest.excludedSources) {
        writeField(out, "excluded_source", excluded);
    }
    for (const auto& interruption : manifest.interruptions) {
        writeField(out, "interruption", interruption);
    }
    for (const auto& source : manifest.sources) {
        out << "source\n";
        writeField(out, "  sourceId", source.sourceId);
        writeField(out, "  participantId", source.participantId);
        writeField(out, "  role", source.role);
        writeField(out, "  origin", source.origin);
        writeField(out, "  fileName", source.fileName);
        writeField(out, "  frames", source.frames);
        writeField(out, "  knownGapFrames", source.knownGapFrames);
        writeField(out, "  sha256", source.sha256);
    }
    return out.str();
}

std::optional<TakeManifest> parseTake(std::string_view text) {
    TakeManifest manifest;
    bool sawHeader = false;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line == "source") {
            manifest.sources.emplace_back();
            continue;
        }
        const auto tab = line.find(separator);
        if (tab == std::string::npos) {
            continue;
        }
        const std::string key = line.substr(0U, tab);
        const std::string value = line.substr(tab + 1U);

        if (key == "jamlink-take") {
            sawHeader = true;
            manifest.manifestVersion = static_cast<std::uint32_t>(parseNumber(value));
        } else if (key == "takeId") {
            manifest.takeId = value;
        } else if (key == "sessionId") {
            manifest.sessionId = value;
        } else if (key == "startedAt") {
            manifest.startedAtMillisecond = parseNumber(value);
        } else if (key == "endedAt") {
            manifest.endedAtMillisecond = parseNumber(value);
        } else if (key == "sampleRate") {
            manifest.sampleRate = static_cast<std::uint32_t>(parseNumber(value));
        } else if (key == "applicationVersion") {
            manifest.applicationVersion = value;
        } else if (key == "mediaFormat") {
            manifest.mediaFormat = value;
        } else if (key == "state") {
            // An unrecognised state is treated as needing review rather than as
            // Ready. A reader that does not understand what it is looking at
            // must not call a recording clean.
            manifest.state =
                takeStateFromName(value).value_or(TakeState::RecoveredNeedsReview);
        } else if (key == "droppedFrames") {
            manifest.droppedFrames = parseNumber(value);
        } else if (key == "excluded_source") {
            manifest.excludedSources.push_back(value);
        } else if (key == "interruption") {
            manifest.interruptions.push_back(value);
        } else if (!manifest.sources.empty()) {
            TakeSource& source = manifest.sources.back();
            if (key == "  sourceId") { source.sourceId = value; }
            else if (key == "  participantId") { source.participantId = value; }
            else if (key == "  role") { source.role = value; }
            else if (key == "  origin") { source.origin = value; }
            else if (key == "  fileName") { source.fileName = value; }
            else if (key == "  frames") { source.frames = parseNumber(value); }
            else if (key == "  knownGapFrames") { source.knownGapFrames = parseNumber(value); }
            else if (key == "  sha256") { source.sha256 = value; }
        }
    }
    if (!sawHeader) {
        return std::nullopt;
    }
    return manifest;
}

namespace {

// Written beside the manifest and renamed over it, so a process killed
// mid-write leaves either the previous manifest or the new one. A partly
// written manifest is worse than a stale one: it would make a real take
// unreadable.
[[nodiscard]] bool writeAtomically(
    const std::filesystem::path& takeDirectory, const TakeManifest& manifest) {
    std::error_code error;
    std::filesystem::create_directories(takeDirectory, error);
    const auto target = takeDirectory / TakeJournal::manifestFileName;
    const auto temporary = takeDirectory / (std::string(TakeJournal::manifestFileName) + ".new");
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << serialiseTake(manifest);
        out.flush();
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(temporary, target, error);
    if (error) {
        // Some filesystems refuse a rename onto an existing file. Falling back
        // is less safe, so it is only used when the atomic path is unavailable.
        std::filesystem::remove(target, error);
        std::filesystem::rename(temporary, target, error);
    }
    return !error;
}

} // namespace

bool TakeJournal::begin(
    const std::filesystem::path& takeDirectory, const TakeManifest& manifest) {
    TakeManifest starting = manifest;
    if (starting.state == TakeState::Preparing) {
        starting.state = TakeState::Recording;
    }
    return writeAtomically(takeDirectory, starting);
}

bool TakeJournal::checkpoint(
    const std::filesystem::path& takeDirectory, const TakeManifest& manifest) {
    return writeAtomically(takeDirectory, manifest);
}

bool TakeJournal::finalise(
    const std::filesystem::path& takeDirectory, TakeManifest manifest) {
    // Ready is only reached by a write that succeeded. If this fails the
    // manifest still says the take was in progress, and startup will find it.
    manifest.state = TakeState::Ready;
    return writeAtomically(takeDirectory, manifest);
}

std::optional<TakeManifest> TakeJournal::read(const std::filesystem::path& takeDirectory) {
    std::ifstream in(takeDirectory / manifestFileName, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream contents;
    contents << in.rdbuf();
    return parseTake(contents.str());
}

std::optional<TakeManifest> TakeJournal::recover(const std::filesystem::path& takeDirectory) {
    auto manifest = read(takeDirectory);
    if (!manifest) {
        return std::nullopt;
    }
    if (manifest->state == TakeState::Ready) {
        return manifest;
    }
    // Everything on disk is kept. The take is marked as needing review and the
    // reason is recorded, because an interrupted recording that opens cleanly
    // is exactly the one a musician would otherwise trust.
    manifest->state = TakeState::RecoveredNeedsReview;
    manifest->interruptions.push_back(
        "JamLink stopped before this take was finalised; its ending may be missing");
    static_cast<void>(writeAtomically(takeDirectory, *manifest));
    return manifest;
}

std::vector<std::filesystem::path> TakeJournal::findInterrupted(
    const std::filesystem::path& root) {
    std::vector<std::filesystem::path> interrupted;
    std::error_code error;
    if (!std::filesystem::exists(root, error)) {
        return interrupted;
    }
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (error) {
            break;
        }
        if (!entry.is_directory(error)) {
            continue;
        }
        const auto manifest = read(entry.path());
        if (manifest && manifest->state != TakeState::Ready
            && manifest->state != TakeState::RecoveredNeedsReview) {
            interrupted.push_back(entry.path());
        }
    }
    std::sort(interrupted.begin(), interrupted.end());
    return interrupted;
}

} // namespace jamlink::record
