// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/diagnostics/support_bundle.hpp"

#include <sstream>
#include <string_view>

namespace jamlink::diagnostics {
namespace {

[[nodiscard]] bool isHex(char character) noexcept {
    return (character >= '0' && character <= '9')
        || (character >= 'a' && character <= 'f')
        || (character >= 'A' && character <= 'F');
}

// A bundle is pasted into chat windows and attached to issues. Anything shaped
// like a key is removed rather than trusted to be absent.
[[nodiscard]] std::string stripSecrets(std::string_view message) {
    std::string result;
    result.reserve(message.size());
    std::size_t hexRun = 0U;
    std::size_t hexStart = 0U;
    for (std::size_t index = 0U; index < message.size(); ++index) {
        const char character = message[index];
        if (isHex(character)) {
            if (hexRun == 0U) {
                hexStart = result.size();
            }
            ++hexRun;
        } else {
            hexRun = 0U;
        }
        result.push_back(character == '\n' || character == '\r' ? ' ' : character);
        // A thirty-two character run of hex is a key or a room secret, not a
        // word any log line would legitimately contain.
        if (hexRun == 32U) {
            result.resize(hexStart);
            result += "<redacted>";
            hexRun = 0U;
            while (index + 1U < message.size() && isHex(message[index + 1U])) {
                ++index;
            }
        }
    }
    return result;
}

// An invite carries the room secret after its last separator. The address is
// worth keeping for diagnosis; the secret never is.
[[nodiscard]] std::string stripInvites(const std::string& line) {
    static constexpr std::string_view marker = "JL1|";
    std::string result = line;
    std::size_t at = result.find(marker);
    while (at != std::string::npos) {
        std::size_t end = at;
        while (end < result.size() && result[end] != ' ' && result[end] != '\t') {
            ++end;
        }
        const std::size_t lastSeparator = result.rfind('|', end);
        const std::size_t keepTo = (lastSeparator != std::string::npos && lastSeparator > at)
            ? lastSeparator + 1U : at + marker.size();
        result = result.substr(0U, keepTo) + "<secret withheld>" + result.substr(end);
        at = result.find(marker, keepTo + 17U);
    }
    return result;
}

void writeField(std::ostringstream& out, std::string_view label, const std::string& value) {
    out << "  " << label << ": " << (value.empty() ? "(unknown)" : value) << '\n';
}

void writeField(std::ostringstream& out, std::string_view label, std::uint64_t value) {
    out << "  " << label << ": " << value << '\n';
}

void writeField(std::ostringstream& out, std::string_view label, bool value) {
    out << "  " << label << ": " << (value ? "yes" : "no") << '\n';
}

[[nodiscard]] std::string jsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 2U);
    for (const char character : value) {
        switch (character) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                result += ' ';
            } else {
                result.push_back(character);
            }
        }
    }
    return result;
}

} // namespace

std::string sanitiseForSupport(std::string_view line) {
    return stripSecrets(stripInvites(std::string(line)));
}

std::string renderSupportBundle(const SupportSnapshot& snapshot) {
    std::ostringstream out;
    out << "JamLink support bundle\n";
    out << "This file contains no room secrets, keys, invites or chat.\n\n";

    out << "Build\n";
    writeField(out, "version", snapshot.applicationVersion);
    writeField(out, "build", snapshot.buildIdentity);
    writeField(out, "channel", snapshot.releaseChannel);
    writeField(out, "media protocol", static_cast<std::uint64_t>(snapshot.mediaProtocolVersion));
    writeField(out, "control protocol", static_cast<std::uint64_t>(snapshot.controlProtocolVersion));

    out << "\nMedia format\n";
    writeField(out, "codec", snapshot.audioCodec);
    writeField(out, "bitrate", static_cast<std::uint64_t>(snapshot.codecBitsPerSecond));
    writeField(out, "frame ms", static_cast<std::uint64_t>(snapshot.codecFrameMilliseconds));
    writeField(out, "opus packets decoded", snapshot.opusPacketsDecoded);
    writeField(out, "uncompressed packets decoded", snapshot.pcmPacketsDecoded);
    writeField(out, "undecodable packets", snapshot.undecodablePackets);
    writeField(out, "encode failures", snapshot.encodeFailures);
    writeField(out, "limited send samples", snapshot.limitedSendSamples);

    out << "\nAudio\n";
    // Device and audio system together, per endpoint. They can differ, and
    // when they do that is usually the whole answer.
    writeField(out, "instrument", snapshot.instrumentDevice);
    writeField(out, "instrument audio system", snapshot.instrumentBackend);
    writeField(out, "voice", snapshot.voiceDevice);
    writeField(out, "voice audio system", snapshot.voiceBackend);
    writeField(out, "output", snapshot.outputDevice);
    writeField(out, "output audio system", snapshot.outputBackend);
    writeField(out, "can run together", snapshot.audioTopologySupported);
    writeField(out, "topology", snapshot.audioTopologyReason);
    writeField(out, "engine running", snapshot.audioRunning);
    // A rate of zero is not a measurement of a device running slowly. Saying
    // so separates "it ran at nothing" from "it never opened", which is the
    // distinction a field bundle failed to make and lost an evening to.
    if (snapshot.audioRunning) {
        writeField(out, "sample rate", static_cast<std::uint64_t>(snapshot.sampleRate));
        writeField(out, "running buffer",
            static_cast<std::uint64_t>(snapshot.runningBufferFrames));
    } else {
        out << "  sample rate: (engine never started)\n";
        out << "  running buffer: (engine never started)\n";
    }
    writeField(out, "requested buffer", static_cast<std::uint64_t>(snapshot.requestedBufferFrames));
    writeField(out, "automatic buffer", snapshot.automaticBufferSize);
    writeField(out, "sound check verified", snapshot.soundCheckVerified);
    writeField(out, "underruns", snapshot.underruns);
    writeField(out, "overruns", snapshot.overruns);

    out << "\nConnection\n";
    writeField(out, "preflight", snapshot.preflightOutcome);
    writeField(out, "mapping protocol", snapshot.portMappingProtocol);
    writeField(out, "mapping result", snapshot.portMappingResult);
    writeField(out, "public address", snapshot.publicAddressDiscovery);
    writeField(out, "firewall", snapshot.firewallState);
    writeField(out, "udp bound", snapshot.udpBound);
    writeField(out, "local udp port", static_cast<std::uint64_t>(snapshot.localUdpPort));
    writeField(out, "router mapping", snapshot.natBehaviour);
    writeField(out, "candidate probes sent", snapshot.candidateProbesSent);
    writeField(out, "candidate rounds exhausted",
        static_cast<std::uint64_t>(snapshot.candidateRoundsExhausted));

    out << "\nLink\n";
    writeField(out, "round trip measured", snapshot.roundTripMeasured);
    writeField(out, "round trip ms", static_cast<std::uint64_t>(snapshot.roundTripMilliseconds));
    writeField(out, "receive buffer ms",
        static_cast<std::uint64_t>(snapshot.receiveBufferMilliseconds));
    writeField(out, "jitter us", static_cast<std::uint64_t>(snapshot.jitterMicroseconds));
    writeField(out, "packets sent", snapshot.packetsSent);
    writeField(out, "packets received", snapshot.packetsReceived);
    writeField(out, "packets concealed", snapshot.packetsConcealed);
    writeField(out, "packets late", snapshot.packetsLate);
    writeField(out, "reconnects", snapshot.reconnectCount);

    out << "\nRecording\n";
    writeField(out, "recording", snapshot.recording);
    writeField(out, "dropped frames", snapshot.recorderDroppedFrames);

    out << "\nSession lifecycle\n";
    if (snapshot.lifecycleTransitions.empty()) {
        out << "  (none)\n";
    }
    for (const auto& transition : snapshot.lifecycleTransitions) {
        out << "  " << sanitiseForSupport(transition) << '\n';
    }

    out << "\nRecent log\n";
    if (snapshot.logExcerpt.empty()) {
        out << "  (none)\n";
    }
    for (const auto& line : snapshot.logExcerpt) {
        out << "  " << sanitiseForSupport(line) << '\n';
    }
    return out.str();
}

std::string renderSupportBundleJson(const SupportSnapshot& snapshot) {
    std::ostringstream out;
    const auto text = [&out](std::string_view key, const std::string& value) {
        out << "\"" << key << "\":\"" << jsonEscape(value) << "\",";
    };
    const auto number = [&out](std::string_view key, std::uint64_t value) {
        out << "\"" << key << "\":" << value << ",";
    };
    const auto flag = [&out](std::string_view key, bool value) {
        out << "\"" << key << "\":" << (value ? "true" : "false") << ",";
    };

    out << "{";
    text("applicationVersion", snapshot.applicationVersion);
    text("buildIdentity", snapshot.buildIdentity);
    text("releaseChannel", snapshot.releaseChannel);
    number("mediaProtocolVersion", snapshot.mediaProtocolVersion);
    number("controlProtocolVersion", snapshot.controlProtocolVersion);
    text("audioCodec", snapshot.audioCodec);
    number("codecBitsPerSecond", snapshot.codecBitsPerSecond);
    number("codecFrameMilliseconds", snapshot.codecFrameMilliseconds);
    number("opusPacketsDecoded", snapshot.opusPacketsDecoded);
    number("pcmPacketsDecoded", snapshot.pcmPacketsDecoded);
    number("undecodablePackets", snapshot.undecodablePackets);
    number("encodeFailures", snapshot.encodeFailures);
    number("limitedSendSamples", snapshot.limitedSendSamples);
    text("instrumentDevice", snapshot.instrumentDevice);
    text("instrumentBackend", snapshot.instrumentBackend);
    text("voiceDevice", snapshot.voiceDevice);
    text("voiceBackend", snapshot.voiceBackend);
    text("outputDevice", snapshot.outputDevice);
    text("outputBackend", snapshot.outputBackend);
    flag("audioTopologySupported", snapshot.audioTopologySupported);
    text("audioTopologyReason", snapshot.audioTopologyReason);
    flag("audioRunning", snapshot.audioRunning);
    number("sampleRate", snapshot.sampleRate);
    number("requestedBufferFrames", snapshot.requestedBufferFrames);
    number("runningBufferFrames", snapshot.runningBufferFrames);
    flag("automaticBufferSize", snapshot.automaticBufferSize);
    flag("soundCheckVerified", snapshot.soundCheckVerified);
    number("underruns", snapshot.underruns);
    number("overruns", snapshot.overruns);
    text("preflightOutcome", snapshot.preflightOutcome);
    text("portMappingProtocol", snapshot.portMappingProtocol);
    text("portMappingResult", snapshot.portMappingResult);
    text("publicAddressDiscovery", snapshot.publicAddressDiscovery);
    text("firewallState", snapshot.firewallState);
    flag("udpBound", snapshot.udpBound);
    number("localUdpPort", snapshot.localUdpPort);
    text("natBehaviour", snapshot.natBehaviour);
    number("candidateProbesSent", snapshot.candidateProbesSent);
    number("candidateRoundsExhausted", snapshot.candidateRoundsExhausted);
    flag("roundTripMeasured", snapshot.roundTripMeasured);
    number("roundTripMilliseconds", snapshot.roundTripMilliseconds);
    number("receiveBufferMilliseconds", snapshot.receiveBufferMilliseconds);
    number("jitterMicroseconds", snapshot.jitterMicroseconds);
    number("packetsSent", snapshot.packetsSent);
    number("packetsReceived", snapshot.packetsReceived);
    number("packetsConcealed", snapshot.packetsConcealed);
    number("packetsLate", snapshot.packetsLate);
    number("reconnectCount", snapshot.reconnectCount);
    flag("recording", snapshot.recording);
    number("recorderDroppedFrames", snapshot.recorderDroppedFrames);

    out << "\"lifecycleTransitions\":[";
    for (std::size_t index = 0U; index < snapshot.lifecycleTransitions.size(); ++index) {
        out << (index == 0U ? "" : ",") << "\""
            << jsonEscape(sanitiseForSupport(snapshot.lifecycleTransitions[index])) << "\"";
    }
    out << "],\"logExcerpt\":[";
    for (std::size_t index = 0U; index < snapshot.logExcerpt.size(); ++index) {
        out << (index == 0U ? "" : ",") << "\""
            << jsonEscape(sanitiseForSupport(snapshot.logExcerpt[index])) << "\"";
    }
    out << "]}";
    return out.str();
}

} // namespace jamlink::diagnostics
