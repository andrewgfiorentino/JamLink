// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jamlink::diagnostics {

// One deliberate artefact a musician can send when something went wrong.
//
// The design rule is that this is an allowlist and not a filter. Everything the
// bundle can contain is a typed field declared below, so a secret cannot leak
// by being present somewhere nobody thought to strip: there is nowhere to put
// it. Nothing here walks a directory, reads the environment, or serialises an
// object that might grow a sensitive member later.
//
// Free text appears in exactly one place -- the log excerpt -- and that is the
// one surface the sanitiser has to defend, which is why it is a single bounded
// field rather than a stream.
struct SupportSnapshot final {
    // Build and protocol.
    std::string applicationVersion;
    std::string buildIdentity;
    std::string releaseChannel;
    std::uint16_t mediaProtocolVersion{0U};
    std::uint16_t controlProtocolVersion{0U};

    // Media format actually in use, which is the question a bad-audio report
    // most often turns on.
    std::string audioCodec;
    std::uint32_t codecBitsPerSecond{0U};
    std::uint32_t codecFrameMilliseconds{0U};
    std::uint64_t opusPacketsDecoded{0U};
    std::uint64_t pcmPacketsDecoded{0U};
    std::uint64_t undecodablePackets{0U};
    std::uint64_t encodeFailures{0U};
    // Samples the send limiter held below full scale on the way out. Zero is
    // the normal state; a large number means the input gain is too high and
    // the friend has been hearing the ceiling.
    std::uint64_t limitedSendSamples{0U};
    // How far the send rate has had to be walked down for the far end, and
    // whether it ran out of room. The bitrate above is what is being sent now
    // rather than what the build starts at.
    std::uint32_t bitrateReductions{0U};
    bool uplinkExhausted{false};

    // Audio path. Device identities are names, never file paths.
    //
    // Each endpoint carries its own audio system. A single "backend" line used
    // to stand for all three, and it was judged from the output alone: a field
    // failure whose guitar was on an interface driver while its headphones went
    // through Windows was reported as a plain Windows setup, which is what hid
    // the fault. Three endpoints that can differ need three answers.
    std::string instrumentDevice;
    std::string instrumentBackend;
    std::string voiceDevice;
    std::string voiceBackend;
    std::string outputDevice;
    std::string outputBackend;
    // Whether those three can run together at all, and the named reason. This
    // is the verdict the audio service itself acts on, not a re-derivation.
    bool audioTopologySupported{false};
    std::string audioTopologyReason;
    // Whether the engine is actually running. Without it a sample rate of 0
    // and a buffer of 0 read as measurements of a running device rather than
    // as the absence of one.
    bool audioRunning{false};
    std::uint32_t sampleRate{0U};
    std::uint32_t requestedBufferFrames{0U};
    std::uint32_t runningBufferFrames{0U};
    bool automaticBufferSize{false};
    bool soundCheckVerified{false};
    std::uint64_t underruns{0U};
    std::uint64_t overruns{0U};

    // Connection. What was attempted and what answered, never where.
    std::string preflightOutcome;
    std::string portMappingProtocol;
    std::string portMappingResult;
    std::string publicAddressDiscovery;
    std::string firewallState;
    bool udpBound{false};
    std::uint16_t localUdpPort{0U};
    // What the router does to this machine on the way out. Named rather than
    // inferred from a failure, because "an invite from here cannot work" and
    // "nobody tried yet" produce the same silence.
    std::string natBehaviour;
    // Path finding, which is what a "we could never connect" report turns on.
    std::uint64_t candidateProbesSent{0U};
    std::uint32_t candidateRoundsExhausted{0U};

    // Link health.
    bool roundTripMeasured{false};
    std::uint32_t roundTripMilliseconds{0U};
    std::uint32_t receiveBufferMilliseconds{0U};
    std::uint32_t jitterMicroseconds{0U};
    std::uint64_t packetsSent{0U};
    std::uint64_t packetsReceived{0U};
    std::uint64_t packetsConcealed{0U};
    std::uint64_t packetsLate{0U};
    std::uint64_t reconnectCount{0U};

    // Recording.
    bool recording{false};
    std::uint64_t recorderDroppedFrames{0U};

    // Session lifecycle, as semantic phase names only.
    std::vector<std::string> lifecycleTransitions;

    // The only free text. Already sanitised on the way in.
    std::vector<std::string> logExcerpt;
};

// Strips anything that looks like a secret from a line of free text.
//
// Shared with the session log rather than reimplemented, so a redaction rule
// added in one place cannot be missing from the other. A thirty-two character
// run of hex is a key or a room secret, not a word.
[[nodiscard]] std::string sanitiseForSupport(std::string_view line);

// Human-readable, for the clipboard and for a preview.
[[nodiscard]] std::string renderSupportBundle(const SupportSnapshot& snapshot);

// The same facts as JSON. Rendered from the same snapshot deliberately: two
// renderers over one source of truth cannot disagree about what happened, which
// they could if each gathered its own.
[[nodiscard]] std::string renderSupportBundleJson(const SupportSnapshot& snapshot);

} // namespace jamlink::diagnostics
