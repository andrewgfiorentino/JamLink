// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "jamlink/record/take_manifest.hpp"

#include <string>

namespace jamlink::record {

// Turns a take into something that opens.
//
// A finished take is a folder of WAV files that all start at the same instant.
// That is the right thing to write -- separate tracks are the whole reason a
// take is worth keeping -- but it leaves the musician to drag six files onto a
// timeline and trust that they line up, which is exactly the moment a good
// recording gets ruined by being nudged.
//
// The alignment is a fact JamLink already knows for certain, so it should be
// written down rather than reconstructed by hand.
//
// Reaper's project format is chosen because it is plain text, small enough that
// a generator for it is obviously correct, and read by more than just Reaper.
// A binary session format would be a much larger surface for a much smaller
// gain.
//
// A pure function of the manifest: no filesystem, so what it produces can be
// checked exactly rather than by opening it and looking.
[[nodiscard]] std::string writeReaperProject(const TakeManifest& manifest);

// The name a musician sees on the track, built from who the source belongs to
// and what it is. "Andrew - guitar (played)" says more than "instrument.wav",
// and the distinction between what was played and what arrived is the one most
// worth having on screen while mixing.
[[nodiscard]] std::string trackNameFor(
    const TakeSource& source,
    const std::string& localName,
    const std::string& remoteName);

} // namespace jamlink::record
