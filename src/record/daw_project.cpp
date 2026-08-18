// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/record/daw_project.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <string_view>

namespace jamlink::record {
namespace {

// The project format quotes strings, so a name carrying a quote would end the
// field early and produce a file the DAW cannot open. Display names come from
// a profile the musician typed, so this is reachable rather than theoretical.
[[nodiscard]] std::string quoteSafe(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(character == '"' ? '\'' : character);
    }
    return result;
}

[[nodiscard]] std::string roleWord(const TakeSource& source) {
    if (source.role == "voice") {
        return "voice";
    }
    if (source.role == "instrument") {
        return "guitar";
    }
    return source.role.empty() ? std::string("audio") : source.role;
}

} // namespace

std::string trackNameFor(
    const TakeSource& source,
    const std::string& localName,
    const std::string& remoteName) {
    const bool received = source.origin == "network-received";
    std::string who = received ? remoteName : localName;
    if (who.empty()) {
        // Never an empty label. A track with no name is one a musician has to
        // solo to identify, which is the work this file exists to remove.
        who = received ? "Your friend" : "You";
    }
    std::string qualifier;
    if (source.origin == "local-original") {
        // The distinction worth having on screen: one of these is what the
        // network delivered and the other is what was actually played.
        qualifier = " (played)";
    } else if (received) {
        qualifier = " (received)";
    } else {
        qualifier = " (heard)";
    }
    return who + " - " + roleWord(source) + qualifier;
}

std::string writeReaperProject(const TakeManifest& manifest) {
    std::string localName;
    std::string remoteName;
    for (const auto& source : manifest.sources) {
        if (source.origin == "network-received") {
            if (remoteName.empty()) {
                remoteName = source.participantId;
            }
        } else if (localName.empty()) {
            localName = source.participantId;
        }
    }

    // Every track starts at the same instant, which is the fact worth writing
    // down. The length comes from the take rather than from any one file: the
    // local originals run at their own capture rate, so a frame count from one
    // of them would misdescribe the others.
    const double seconds = manifest.endedAtMillisecond > manifest.startedAtMillisecond
        ? static_cast<double>(
              manifest.endedAtMillisecond - manifest.startedAtMillisecond) / 1'000.0
        : 0.0;

    std::ostringstream out;
    out << "<REAPER_PROJECT 0.1 \"7.0\" 0\n";
    out << "  RIPPLE 0\n";
    out << "  SAMPLERATE " << (manifest.sampleRate == 0U ? 48'000U : manifest.sampleRate)
        << " 0 0\n";
    out << "  TEMPO 120 4 4\n";
    for (const auto& source : manifest.sources) {
        if (source.fileName.empty()) {
            continue;
        }
        out << "  <TRACK\n";
        out << "    NAME \"" << quoteSafe(trackNameFor(source, localName, remoteName))
            << "\"\n";
        out << "    VOLPAN 1 0 -1 -1 1\n";
        out << "    <ITEM\n";
        out << "      POSITION 0\n";
        out << "      LENGTH " << seconds << "\n";
        out << "      NAME \"" << quoteSafe(source.fileName) << "\"\n";
        out << "      <SOURCE WAVE\n";
        out << "        FILE \"" << quoteSafe(source.fileName) << "\"\n";
        out << "      >\n";
        out << "    >\n";
        out << "  >\n";
    }
    out << ">\n";
    return out.str();
}

} // namespace jamlink::record
