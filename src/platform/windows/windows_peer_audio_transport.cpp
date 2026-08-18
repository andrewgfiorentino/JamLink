// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <Windows.h>

#ifndef SIO_UDP_CONNRESET
// Documented Winsock control code that is absent from some SDK header sets.
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif

#include "jamlink/audio/async_mono_resampler.hpp"
#include "jamlink/diagnostics/session_log.hpp"
#include "jamlink/audio/gain_stage.hpp"
#include "jamlink/audio/send_limiter.hpp"
#include "jamlink/audio/realtime_atomic.hpp"
#include "jamlink/audio/spsc_audio_ring.hpp"
#include "jamlink/network/audio_stream_receiver.hpp"
#include "jamlink/control/room_capacity.hpp"
#include "jamlink/network/bitrate_controller.hpp"
#include "jamlink/network/peer_key_schedule.hpp"
#include "jamlink/network/room_roster.hpp"
#include "jamlink/network/ice_agent.hpp"
#include "jamlink/network/nat_behaviour.hpp"
#include "jamlink/network/outgoing_audio_pacer.hpp"
#include "jamlink/network/peer_audio_codec.hpp"
#include "jamlink/network/peer_audio_transport.hpp"

#include <bcrypt.h>
#include <iphlpapi.h>
#include <natupnp.h>
#include <objbase.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace jamlink::network {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::size_t headerBytes = 32U;
constexpr std::size_t tagBytes = 16U;
constexpr std::size_t maximumPlaintextBytes = 960U;
constexpr std::size_t maximumDatagramBytes = headerBytes + maximumPlaintextBytes + tagBytes;
constexpr std::uint32_t protocolMagic = 0x4A4C4B31U; // JLK1
// 3 frames every audio payload with the codec that produced it.
constexpr std::uint8_t protocolVersion = 3U;
constexpr std::uint32_t networkSampleRate = 48'000U;
constexpr std::size_t networkPacketFrames = 240U;
constexpr std::size_t noncePrefixBytes = 8U;
// Two hours. Long enough for a jam, short enough that an abandoned mapping
// does not sit on the router indefinitely.
constexpr std::uint32_t portMappingLifetimeSeconds = 7'200U;
// Two hundred milliseconds of capture waiting to go out. Past this the audio
// is older than a live session can use, so holding it would only delay
// everything behind it.
constexpr std::size_t maximumOutgoingBacklogFrames = 9'600U;
// Enough for either wire format, since a packet names its own.
constexpr std::size_t maximumAudioPayloadBytes = codecTagBytes + 1'275U;
// Chosen for an instrument rather than for speech. Uncompressed is 768 kbit/s
// per stream, so this is a little over an eighth of it.
constexpr std::uint32_t outgoingBitsPerSecond = 96'000U;
constexpr std::uint32_t maximumNonceCounter = 0xFFFFFF00U;
constexpr std::uint8_t streamIndexMask = 0x7FU;
constexpr std::uint8_t sourceClipFlag = 0x80U;

enum class PacketType : std::uint8_t {
    Hello = 1U,
    HelloAck = 2U,
    Audio = 3U,
    Ping = 4U,
    Pong = 5U,
    VersionMismatch = 6U,
    Chat = 7U,
    ChatAck = 8U,
    // Sent by the host toward a known guest endpoint before a session exists.
    // A home router only forwards inbound UDP for an endpoint it has already
    // seen outbound traffic to, so a host that transmits nothing until it
    // receives can never be reached: the guest's Hello is discarded by the
    // router before JamLink sees it. Punching opens that mapping from both
    // sides at once. It carries no payload and changes no session state.
    Punch = 9U,
    // Where the sender can be reached, sent once a session is up.
    //
    // Only the host publishes addresses today, in the invite, because only the
    // host has to be found. In a mesh everybody has to be findable by everybody
    // else, and the room's creator is the one participant guaranteed to know
    // them all -- so guests tell it where they are, and it will have something
    // real to introduce people with.
    Candidates = 10U,
    // The room cannot carry another musician. Sent in answer to a join
    // request that found no slot, because silence is indistinguishable from a
    // network that never carried them -- somebody refused has to be told they
    // were refused rather than left watching a spinner.
    //
    // Sealed with the room key rather than a pair key, exactly like the join
    // request it answers: admission happens before a pair exists, so there is
    // no pair key to use. That puts it on the same trust boundary as the
    // Hello -- anybody who can forge one already holds the room secret and is
    // therefore already in the room.
    RoomFull = 11U
};

// Both peers know the same room secret, so a single key would let an attacker
// reflect a peer's own authenticated packets back at it, and would leave two
// independent nonce sequences under one key. Each direction gets its own key
// derived from the secret instead.
enum class Direction : std::uint8_t {
    HostToGuest = 1U,
    GuestToHost = 2U
};

[[nodiscard]] bool hmacSha256(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> message,
    std::span<std::uint8_t, 32U> digest) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG))) {
        return false;
    }
    const bool succeeded = BCRYPT_SUCCESS(BCryptHash(
        algorithm,
        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()),
        const_cast<PUCHAR>(message.data()), static_cast<ULONG>(message.size()),
        digest.data(), static_cast<ULONG>(digest.size())));
    static_cast<void>(BCryptCloseAlgorithmProvider(algorithm, 0U));
    return succeeded;
}

// One HMAC-SHA256 invocation with a distinct label per direction. This is
// HKDF-Expand with a single output block, not a bespoke construction.
[[nodiscard]] bool deriveDirectionKey(
    std::span<const std::uint8_t, 32U> secret,
    Direction direction,
    std::span<std::uint8_t, 32U> key) noexcept {
    static constexpr char hostToGuest[] = "JamLink JL1 protocol 2 host-to-guest audio key";
    static constexpr char guestToHost[] = "JamLink JL1 protocol 2 guest-to-host audio key";
    const char* label = direction == Direction::HostToGuest ? hostToGuest : guestToHost;
    const std::size_t labelBytes = direction == Direction::HostToGuest
        ? sizeof(hostToGuest) - 1U
        : sizeof(guestToHost) - 1U;
    return hmacSha256(
        secret,
        std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(label), labelBytes),
        key);
}

// Sliding replay window. A strictly increasing counter would also reject every
// reordered packet, which the network genuinely produces and the receive
// jitter buffer is able to use.
class ReplayWindow final {
public:
    [[nodiscard]] bool accept(std::uint32_t sequence) noexcept {
        if (!started_) {
            started_ = true;
            highest_ = sequence;
            bitmap_ = 1U;
            return true;
        }
        const auto delta = static_cast<std::int32_t>(sequence - highest_);
        if (delta > 0) {
            bitmap_ = delta >= static_cast<std::int32_t>(windowBits)
                ? 1U
                : ((bitmap_ << static_cast<unsigned>(delta)) | 1U);
            highest_ = sequence;
            return true;
        }
        const auto behind = static_cast<std::uint32_t>(-delta);
        if (behind >= windowBits) {
            return false;
        }
        const std::uint64_t mask = std::uint64_t{1} << behind;
        if ((bitmap_ & mask) != 0U) {
            return false;
        }
        bitmap_ |= mask;
        return true;
    }

    void reset() noexcept {
        started_ = false;
        highest_ = 0U;
        bitmap_ = 0U;
    }

private:
    static constexpr std::uint32_t windowBits = 64U;
    std::uint32_t highest_{0U};
    std::uint64_t bitmap_{0U};
    bool started_{false};
};

class WinsockLifetime final {
public:
    WinsockLifetime() noexcept {
        WSADATA data{};
        available_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WinsockLifetime() {
        if (available_) {
            static_cast<void>(WSACleanup());
        }
    }
    [[nodiscard]] bool available() const noexcept { return available_; }

private:
    bool available_{false};
};

class SocketHandle final {
public:
    SocketHandle() = default;
    explicit SocketHandle(SOCKET value) noexcept : value_(value) {}
    ~SocketHandle() { reset(); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept
        : value_(std::exchange(other.value_, INVALID_SOCKET)) {}
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this != &other) {
            reset();
            value_ = std::exchange(other.value_, INVALID_SOCKET);
        }
        return *this;
    }
    [[nodiscard]] SOCKET get() const noexcept { return value_; }
    [[nodiscard]] explicit operator bool() const noexcept { return value_ != INVALID_SOCKET; }
    void reset(SOCKET replacement = INVALID_SOCKET) noexcept {
        if (value_ != INVALID_SOCKET) {
            static_cast<void>(closesocket(value_));
        }
        value_ = replacement;
    }

private:
    SOCKET value_{INVALID_SOCKET};
};

class ComApartment final {
public:
    ComApartment() noexcept {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = result == S_OK || result == S_FALSE;
        available_ = initialized_ || result == RPC_E_CHANGED_MODE;
    }
    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }
    [[nodiscard]] bool available() const noexcept { return available_; }

private:
    bool initialized_{false};
    bool available_{false};
};

[[nodiscard]] std::uint16_t readU16(const std::uint8_t* data) noexcept {
    std::uint16_t value = 0U;
    std::memcpy(&value, data, sizeof(value));
    return ntohs(value);
}

[[nodiscard]] std::uint32_t readU32(const std::uint8_t* data) noexcept {
    std::uint32_t value = 0U;
    std::memcpy(&value, data, sizeof(value));
    return ntohl(value);
}

void writeU16(std::uint8_t* data, std::uint16_t value) noexcept {
    value = htons(value);
    std::memcpy(data, &value, sizeof(value));
}

void writeU32(std::uint8_t* data, std::uint32_t value) noexcept {
    value = htonl(value);
    std::memcpy(data, &value, sizeof(value));
}

[[nodiscard]] std::uint64_t readU64(const std::uint8_t* data) noexcept {
    return static_cast<std::uint64_t>(readU32(data))
        | (static_cast<std::uint64_t>(readU32(data + 4U)) << 32U);
}

void writeU64(std::uint8_t* data, std::uint64_t value) noexcept {
    writeU32(data, static_cast<std::uint32_t>(value));
    writeU32(data + 4U, static_cast<std::uint32_t>(value >> 32U));
}

[[nodiscard]] bool validUtf8(std::string_view text, bool allowNewlines) noexcept {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(text.data());
    std::size_t index = 0U;
    while (index < text.size()) {
        const std::uint8_t first = bytes[index];
        if (first < 0x80U) {
            if ((first < 0x20U && first != '\t' && (!allowNewlines || first != '\n'))
                || first == 0x7FU) {
                return false;
            }
            ++index;
            continue;
        }
        std::size_t continuation = 0U;
        std::uint32_t codePoint = 0U;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation = 1U;
            codePoint = first & 0x1FU;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation = 2U;
            codePoint = first & 0x0FU;
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation = 3U;
            codePoint = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation >= text.size()) {
            return false;
        }
        for (std::size_t offset = 1U; offset <= continuation; ++offset) {
            const std::uint8_t next = bytes[index + offset];
            if ((next & 0xC0U) != 0x80U) {
                return false;
            }
            codePoint = (codePoint << 6U) | (next & 0x3FU);
        }
        if ((continuation == 2U && codePoint < 0x800U)
            || (continuation == 3U && codePoint < 0x10000U)
            || (codePoint >= 0xD800U && codePoint <= 0xDFFFU)
            || codePoint > 0x10FFFFU) {
            return false;
        }
        index += continuation + 1U;
    }
    return true;
}

[[nodiscard]] bool validParticipant(const PeerParticipantInfo& value) noexcept {
    const auto validField = [](const std::string& field, std::size_t maximum) {
        return !field.empty() && field.size() <= maximum && validUtf8(field, false);
    };
    return validField(value.profileId, 64U)
        && value.handle.size() <= 32U && validUtf8(value.handle, false)
        && validField(value.displayName, 64U)
        && validField(value.avatarId, 64U)
        && validField(value.primaryInstrument, 32U)
        && validField(value.applicationVersion, 32U)
        && validField(value.buildIdentity, 64U)
        && validField(value.releaseChannel, 16U)
        && value.mediaProtocolVersion != 0U
        && value.controlProtocolVersion != 0U;
}

[[nodiscard]] bool appendField(
    std::span<std::uint8_t> destination,
    std::size_t& offset,
    const std::string& value) noexcept {
    if (value.size() > 255U || offset + 1U + value.size() > destination.size()) {
        return false;
    }
    destination[offset++] = static_cast<std::uint8_t>(value.size());
    std::memcpy(destination.data() + offset, value.data(), value.size());
    offset += value.size();
    return true;
}

[[nodiscard]] std::size_t encodeParticipant(
    const PeerParticipantInfo& participant,
    std::span<std::uint8_t> destination) noexcept {
    if (!validParticipant(participant) || destination.size() < 4U) {
        return 0U;
    }
    writeU16(destination.data(), participant.mediaProtocolVersion);
    writeU16(destination.data() + 2U, participant.controlProtocolVersion);
    std::size_t offset = 4U;
    if (!appendField(destination, offset, participant.profileId)
        || !appendField(destination, offset, participant.handle)
        || !appendField(destination, offset, participant.displayName)
        || !appendField(destination, offset, participant.avatarId)
        || !appendField(destination, offset, participant.primaryInstrument)
        || !appendField(destination, offset, participant.applicationVersion)
        || !appendField(destination, offset, participant.buildIdentity)
        || !appendField(destination, offset, participant.releaseChannel)) {
        return 0U;
    }
    return offset;
}

[[nodiscard]] bool readField(
    std::span<const std::uint8_t> source,
    std::size_t& offset,
    std::string& value) {
    if (offset >= source.size()) {
        return false;
    }
    const std::size_t bytes = source[offset++];
    if (offset + bytes > source.size()) {
        return false;
    }
    value.assign(reinterpret_cast<const char*>(source.data() + offset), bytes);
    offset += bytes;
    return true;
}

[[nodiscard]] bool decodeParticipant(
    std::span<const std::uint8_t> source,
    PeerParticipantInfo& participant) {
    if (source.size() < 4U) {
        return false;
    }
    participant = {};
    participant.mediaProtocolVersion = readU16(source.data());
    participant.controlProtocolVersion = readU16(source.data() + 2U);
    std::size_t offset = 4U;
    return readField(source, offset, participant.profileId)
        && readField(source, offset, participant.handle)
        && readField(source, offset, participant.displayName)
        && readField(source, offset, participant.avatarId)
        && readField(source, offset, participant.primaryInstrument)
        && readField(source, offset, participant.applicationVersion)
        && readField(source, offset, participant.buildIdentity)
        && readField(source, offset, participant.releaseChannel)
        && offset == source.size() && validParticipant(participant);
}

[[nodiscard]] bool compatibleParticipants(
    const PeerParticipantInfo& local,
    const PeerParticipantInfo& remote) noexcept {
    return local.mediaProtocolVersion == remote.mediaProtocolVersion
        && local.controlProtocolVersion == remote.controlProtocolVersion
        && local.applicationVersion == remote.applicationVersion
        && local.buildIdentity == remote.buildIdentity
        && local.releaseChannel == remote.releaseChannel;
}

[[nodiscard]] std::string hexEncode(std::span<const std::uint8_t> bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        result[index * 2U] = digits[bytes[index] >> 4U];
        result[index * 2U + 1U] = digits[bytes[index] & 0x0FU];
    }
    return result;
}

[[nodiscard]] bool hexDecode(
    const std::string& text,
    std::span<std::uint8_t> destination) noexcept {
    if (text.size() != destination.size() * 2U) {
        return false;
    }
    const auto value = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (std::size_t index = 0U; index < destination.size(); ++index) {
        const int high = value(text[index * 2U]);
        const int low = value(text[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        destination[index] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return true;
}

class AesGcmCipher final {
public:
    explicit AesGcmCipher(std::span<const std::uint8_t, 32U> secret) {
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(
                &algorithm_, BCRYPT_AES_ALGORITHM, nullptr, 0U))) {
            return;
        }
        if (!BCRYPT_SUCCESS(BCryptSetProperty(
                algorithm_, BCRYPT_CHAINING_MODE,
                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
                static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_GCM)), 0U))) {
            return;
        }
        ULONG resultBytes = 0U;
        ULONG objectBytes = 0U;
        if (!BCRYPT_SUCCESS(BCryptGetProperty(
                algorithm_, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                &resultBytes, 0U))) {
            return;
        }
        keyObject_.resize(objectBytes);
        if (!BCRYPT_SUCCESS(BCryptGenerateSymmetricKey(
                algorithm_, &key_, keyObject_.data(), objectBytes,
                const_cast<PUCHAR>(secret.data()), static_cast<ULONG>(secret.size()), 0U))) {
            return;
        }
        valid_ = true;
    }

    ~AesGcmCipher() {
        if (key_ != nullptr) {
            static_cast<void>(BCryptDestroyKey(key_));
        }
        if (algorithm_ != nullptr) {
            static_cast<void>(BCryptCloseAlgorithmProvider(algorithm_, 0U));
        }
    }

    AesGcmCipher(const AesGcmCipher&) = delete;
    AesGcmCipher& operator=(const AesGcmCipher&) = delete;

    [[nodiscard]] bool valid() const noexcept { return valid_; }

    [[nodiscard]] bool encrypt(
        std::span<const std::uint8_t> header,
        std::span<const std::uint8_t> plaintext,
        std::span<std::uint8_t> ciphertext,
        std::span<std::uint8_t, tagBytes> tag) noexcept {
        if (!valid_ || header.size() < headerBytes || ciphertext.size() < plaintext.size()) {
            return false;
        }
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication;
        BCRYPT_INIT_AUTH_MODE_INFO(authentication);
        authentication.pbNonce = const_cast<PUCHAR>(header.data() + 20U);
        authentication.cbNonce = 12U;
        authentication.pbAuthData = const_cast<PUCHAR>(header.data());
        authentication.cbAuthData = static_cast<ULONG>(header.size());
        authentication.pbTag = tag.data();
        authentication.cbTag = static_cast<ULONG>(tag.size());
        ULONG written = 0U;
        return BCRYPT_SUCCESS(BCryptEncrypt(
            key_, const_cast<PUCHAR>(plaintext.data()), static_cast<ULONG>(plaintext.size()),
            &authentication, nullptr, 0U, ciphertext.data(),
            static_cast<ULONG>(ciphertext.size()), &written, 0U))
            && written == plaintext.size();
    }

    [[nodiscard]] bool decrypt(
        std::span<const std::uint8_t> header,
        std::span<const std::uint8_t> ciphertext,
        std::span<const std::uint8_t, tagBytes> tag,
        std::span<std::uint8_t> plaintext) noexcept {
        if (!valid_ || header.size() < headerBytes || plaintext.size() < ciphertext.size()) {
            return false;
        }
        BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO authentication;
        BCRYPT_INIT_AUTH_MODE_INFO(authentication);
        authentication.pbNonce = const_cast<PUCHAR>(header.data() + 20U);
        authentication.cbNonce = 12U;
        authentication.pbAuthData = const_cast<PUCHAR>(header.data());
        authentication.cbAuthData = static_cast<ULONG>(header.size());
        authentication.pbTag = const_cast<PUCHAR>(tag.data());
        authentication.cbTag = static_cast<ULONG>(tag.size());
        ULONG written = 0U;
        return BCRYPT_SUCCESS(BCryptDecrypt(
            key_, const_cast<PUCHAR>(ciphertext.data()), static_cast<ULONG>(ciphertext.size()),
            &authentication, nullptr, 0U, plaintext.data(),
            static_cast<ULONG>(plaintext.size()), &written, 0U))
            && written == ciphertext.size();
    }

private:
    BCRYPT_ALG_HANDLE algorithm_{nullptr};
    BCRYPT_KEY_HANDLE key_{nullptr};
    std::vector<std::uint8_t> keyObject_;
    bool valid_{false};
};

[[nodiscard]] std::string localIpv4Address() {
    SocketHandle routeSocket(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (routeSocket) {
        sockaddr_in routeTarget{};
        routeTarget.sin_family = AF_INET;
        routeTarget.sin_port = htons(53U);
        if (inet_pton(AF_INET, "1.1.1.1", &routeTarget.sin_addr) == 1
            && connect(routeSocket.get(), reinterpret_cast<const sockaddr*>(&routeTarget),
                       sizeof(routeTarget)) == 0) {
            sockaddr_in routedLocal{};
            int routedLocalBytes = sizeof(routedLocal);
            if (getsockname(routeSocket.get(), reinterpret_cast<sockaddr*>(&routedLocal),
                            &routedLocalBytes) == 0) {
                char routedText[INET_ADDRSTRLEN]{};
                if (inet_ntop(AF_INET, &routedLocal.sin_addr,
                              routedText, sizeof(routedText)) != nullptr
                    && std::string(routedText).rfind("127.", 0U) != 0U) {
                    return routedText;
                }
            }
        }
    }

    char hostname[256]{};
    if (gethostname(hostname, static_cast<int>(sizeof(hostname))) != 0) {
        return "127.0.0.1";
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) {
        return "127.0.0.1";
    }
    std::string fallback = "127.0.0.1";
    for (const addrinfo* item = result; item != nullptr; item = item->ai_next) {
        const auto* address = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
        char text[INET_ADDRSTRLEN]{};
        if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr) {
            const std::string candidate(text);
            if (candidate.rfind("127.", 0U) != 0U) {
                fallback = candidate;
                break;
            }
        }
    }
    freeaddrinfo(result);
    return fallback;
}

// Default IPv4 gateway, which is where PCP and NAT-PMP requests go.
[[nodiscard]] std::string defaultGatewayAddress() {
    ULONG size = 0U;
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, nullptr, &size)
        != ERROR_BUFFER_OVERFLOW) {
        return {};
    }
    std::vector<std::uint8_t> storage(size);
    auto* adapters = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(storage.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_GATEWAYS, nullptr, adapters, &size)
        != NO_ERROR) {
        return {};
    }
    for (const IP_ADAPTER_ADDRESSES* adapter = adapters; adapter != nullptr;
         adapter = adapter->Next) {
        if (adapter->OperStatus != IfOperStatusUp
            || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (const IP_ADAPTER_GATEWAY_ADDRESS_LH* gateway = adapter->FirstGatewayAddress;
             gateway != nullptr; gateway = gateway->Next) {
            if (gateway->Address.lpSockaddr == nullptr
                || gateway->Address.lpSockaddr->sa_family != AF_INET) {
                continue;
            }
            const auto* address =
                reinterpret_cast<const sockaddr_in*>(gateway->Address.lpSockaddr);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr) {
                return text;
            }
        }
    }
    return {};
}

// Port Control Protocol (RFC 6887) and its predecessor NAT-PMP (RFC 6886).
// Plenty of routers ship one of these with UPnP switched off, or support them
// where their UPnP implementation is broken, so trying all three materially
// raises the chance that a host is reachable from a single invite.
struct GatewayMapping final {
    bool mapped{false};
    std::string externalAddress;
    std::uint16_t externalPort{0U};
    const char* protocol{""};
};

[[nodiscard]] GatewayMapping requestGatewayMapping(
    std::uint16_t port,
    const std::string& localAddress,
    std::uint32_t lifetimeSeconds) {
    GatewayMapping result;
    const std::string gateway = defaultGatewayAddress();
    if (gateway.empty() || localAddress.empty()) {
        return result;
    }

    SocketHandle probe(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (!probe) {
        return result;
    }
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(5351U);
    if (inet_pton(AF_INET, gateway.c_str(), &target.sin_addr) != 1) {
        return result;
    }

    in_addr local{};
    if (inet_pton(AF_INET, localAddress.c_str(), &local) != 1) {
        return result;
    }

    // PCP MAP: version 2, opcode 1. The nonce ties the response to this request.
    std::array<std::uint8_t, 60U> pcp{};
    pcp[0] = 2U;
    pcp[1] = 1U;
    writeU32(pcp.data() + 4U, lifetimeSeconds);
    // Client address as an IPv4-mapped IPv6 address.
    pcp[18] = 0xFFU;
    pcp[19] = 0xFFU;
    std::memcpy(pcp.data() + 20U, &local.s_addr, 4U);
    std::array<std::uint8_t, 12U> nonce{};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(
            nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        return result;
    }
    std::memcpy(pcp.data() + 24U, nonce.data(), nonce.size());
    pcp[36] = 17U; // UDP
    writeU16(pcp.data() + 40U, port);
    writeU16(pcp.data() + 42U, port);
    pcp[46] = 0xFFU;
    pcp[47] = 0xFFU;

    // NAT-PMP MAP UDP: version 0, opcode 1.
    std::array<std::uint8_t, 12U> natpmp{};
    natpmp[1] = 1U;
    writeU16(natpmp.data() + 4U, port);
    writeU16(natpmp.data() + 6U, port);
    writeU32(natpmp.data() + 8U, lifetimeSeconds);

    struct Attempt final {
        const std::uint8_t* data;
        std::size_t size;
        const char* name;
    };
    const std::array<Attempt, 2U> attempts{{
        {pcp.data(), pcp.size(), "PCP"},
        {natpmp.data(), natpmp.size(), "NAT-PMP"},
    }};

    for (const Attempt& attempt : attempts) {
        if (sendto(
                probe.get(), reinterpret_cast<const char*>(attempt.data),
                static_cast<int>(attempt.size), 0,
                reinterpret_cast<const sockaddr*>(&target), sizeof(target))
            != static_cast<int>(attempt.size)) {
            continue;
        }
        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(probe.get(), &readSet);
        timeval timeout{1, 0};
        if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0) {
            continue;
        }
        std::array<std::uint8_t, 128U> response{};
        const int received = recvfrom(
            probe.get(), reinterpret_cast<char*>(response.data()),
            static_cast<int>(response.size()), 0, nullptr, nullptr);
        if (received < 12) {
            continue;
        }
        // PCP replies carry version 2 and the response bit; NAT-PMP uses
        // version 0. Both report success as result code zero.
        if (response[0] == 2U && received >= 60 && (response[1] & 0x80U) != 0U) {
            if (response[3] != 0U
                || std::memcmp(response.data() + 24U, nonce.data(), nonce.size()) != 0) {
                continue;
            }
            result.externalPort = readU16(response.data() + 42U);
            in_addr external{};
            std::memcpy(&external.s_addr, response.data() + 56U, 4U);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &external, text, sizeof(text)) != nullptr) {
                result.externalAddress = text;
            }
            result.mapped = true;
            result.protocol = attempt.name;
            return result;
        }
        if (response[0] == 0U && response[1] == 129U && received >= 16) {
            if (readU16(response.data() + 2U) != 0U) {
                continue;
            }
            result.externalPort = readU16(response.data() + 10U);
            result.mapped = true;
            result.protocol = attempt.name;
            return result;
        }
    }
    return result;
}

struct PortMappingResult final {
    bool mapped{false};
    std::string externalAddress;
};

[[nodiscard]] PortMappingResult addAutomaticPortMapping(
    std::uint16_t port,
    const std::string& localAddress) {
    PortMappingResult result;
    ComApartment apartment;
    if (!apartment.available()) {
        return result;
    }
    ComPtr<IUPnPNAT> nat;
    if (FAILED(CoCreateInstance(
            __uuidof(UPnPNAT), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nat)))) {
        return result;
    }
    ComPtr<IStaticPortMappingCollection> mappings;
    if (FAILED(nat->get_StaticPortMappingCollection(&mappings)) || !mappings) {
        return result;
    }
    const std::wstring local(localAddress.begin(), localAddress.end());
    BSTR protocol = SysAllocString(L"UDP");
    BSTR client = SysAllocString(local.c_str());
    BSTR description = SysAllocString(L"JamLink two-person audio");
    ComPtr<IStaticPortMapping> mapping;
    const HRESULT added = mappings->Add(
        port, protocol, port, client, VARIANT_TRUE, description, &mapping);
    SysFreeString(description);
    SysFreeString(client);
    SysFreeString(protocol);
    if (FAILED(added) || !mapping) {
        return result;
    }
    BSTR external = nullptr;
    if (SUCCEEDED(mapping->get_ExternalIPAddress(&external)) && external != nullptr) {
        const int characters = static_cast<int>(SysStringLen(external));
        const int bytes = WideCharToMultiByte(
            CP_UTF8, 0, external, characters, nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            std::string value(static_cast<std::size_t>(bytes), '\0');
            static_cast<void>(WideCharToMultiByte(
                CP_UTF8, 0, external, characters, value.data(), bytes, nullptr, nullptr));
            result.externalAddress = std::move(value);
        }
        SysFreeString(external);
    }
    result.mapped = true;
    return result;
}

void removeAutomaticPortMapping(std::uint16_t port) noexcept {
    ComApartment apartment;
    if (!apartment.available()) {
        return;
    }
    ComPtr<IUPnPNAT> nat;
    if (FAILED(CoCreateInstance(
            __uuidof(UPnPNAT), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nat)))) {
        return;
    }
    ComPtr<IStaticPortMappingCollection> mappings;
    if (FAILED(nat->get_StaticPortMappingCollection(&mappings)) || !mappings) {
        return;
    }
    BSTR protocol = SysAllocString(L"UDP");
    static_cast<void>(mappings->Remove(port, protocol));
    SysFreeString(protocol);
}

struct StunEndpoint final {
    bool succeeded{false};
    std::string address;
    std::uint16_t port{0U};
};

// Two servers on purpose. Asking one server twice measures nothing: a
// symmetric router keeps the same port for the same destination, so it would
// look endpoint-independent every time.
inline constexpr const char* primaryStunHost = "stun.cloudflare.com";
inline constexpr const char* secondaryStunHost = "stun.l.google.com";
inline constexpr const char* primaryStunPort = "3478";
inline constexpr const char* secondaryStunPort = "19302";

[[nodiscard]] StunEndpoint queryStun(
    SOCKET socket,
    const char* stunHost = primaryStunHost,
    const char* stunPort = primaryStunPort) {
    StunEndpoint result;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(stunHost, stunPort, &hints, &addresses) != 0) {
        return result;
    }
    std::array<std::uint8_t, 20U> request{};
    writeU16(request.data(), 0x0001U);
    writeU16(request.data() + 2U, 0U);
    writeU32(request.data() + 4U, 0x2112A442U);
    if (!BCRYPT_SUCCESS(BCryptGenRandom(
            nullptr, request.data() + 8U, 12U, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
        freeaddrinfo(addresses);
        return result;
    }
    static_cast<void>(sendto(
        socket, reinterpret_cast<const char*>(request.data()),
        static_cast<int>(request.size()), 0, addresses->ai_addr,
        static_cast<int>(addresses->ai_addrlen)));
    freeaddrinfo(addresses);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(socket, &readSet);
    timeval timeout{1, 0};
    if (select(0, &readSet, nullptr, nullptr, &timeout) <= 0) {
        return result;
    }
    std::array<std::uint8_t, 512U> response{};
    sockaddr_in source{};
    int sourceBytes = sizeof(source);
    const int received = recvfrom(
        socket, reinterpret_cast<char*>(response.data()),
        static_cast<int>(response.size()), 0,
        reinterpret_cast<sockaddr*>(&source), &sourceBytes);
    if (received < 20 || readU16(response.data()) != 0x0101U
        || readU32(response.data() + 4U) != 0x2112A442U
        || std::memcmp(response.data() + 8U, request.data() + 8U, 12U) != 0) {
        return result;
    }
    const std::size_t messageBytes = std::min<std::size_t>(
        static_cast<std::size_t>(received), 20U + readU16(response.data() + 2U));
    for (std::size_t offset = 20U; offset + 4U <= messageBytes;) {
        const std::uint16_t type = readU16(response.data() + offset);
        const std::uint16_t length = readU16(response.data() + offset + 2U);
        const std::size_t valueOffset = offset + 4U;
        if (valueOffset + length > messageBytes) {
            break;
        }
        if ((type == 0x0020U || type == 0x0001U) && length >= 8U
            && response[valueOffset + 1U] == 0x01U) {
            std::uint16_t port = readU16(response.data() + valueOffset + 2U);
            std::uint32_t address = readU32(response.data() + valueOffset + 4U);
            if (type == 0x0020U) {
                port ^= 0x2112U;
                address ^= 0x2112A442U;
            }
            in_addr ipv4{};
            ipv4.s_addr = htonl(address);
            char text[INET_ADDRSTRLEN]{};
            if (inet_ntop(AF_INET, &ipv4, text, sizeof(text)) != nullptr) {
                result = {true, text, port};
                return result;
            }
        }
        offset = valueOffset + ((static_cast<std::size_t>(length) + 3U) & ~3U);
    }
    return result;
}

// Whether this machine can be reached by an invite it creates.
//
// The preflight has always had honest wording for a router that cannot be
// hosted from, and nothing could ever detect one, so the branch was dead and
// the musician got a spinner. Two observations from two servers is the whole
// measurement.
[[nodiscard]] NatAssessment probeNatBehaviour(SOCKET socket, const StunEndpoint& primary) {
    if (!primary.succeeded) {
        // Without a first observation there is nothing to compare against, and
        // a second probe would only cost a second of startup.
        return classifyNatBehaviour(ObservedMapping{}, ObservedMapping{});
    }
    const StunEndpoint secondary =
        queryStun(socket, secondaryStunHost, secondaryStunPort);
    return classifyNatBehaviour(
        ObservedMapping{true, primary.address, primary.port},
        ObservedMapping{secondary.succeeded, secondary.address, secondary.port});
}

[[nodiscard]] bool sameEndpoint(
    const sockaddr_in& left,
    const sockaddr_in& right) noexcept {
    return left.sin_family == right.sin_family
        && left.sin_port == right.sin_port
        && left.sin_addr.s_addr == right.sin_addr.s_addr;
}

// GetTickCount64 resolution is 10 to 16 ms, which is larger than the round
// trips JamLink is trying to report and far larger than a 5 ms packet.
[[nodiscard]] std::uint64_t nowMicroseconds() noexcept {
    static const std::uint64_t frequency = [] {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) != 0
            ? static_cast<std::uint64_t>(value.QuadPart)
            : 1'000'000ULL;
    }();
    LARGE_INTEGER counter{};
    if (QueryPerformanceCounter(&counter) == 0) {
        return 0U;
    }
    const auto ticks = static_cast<std::uint64_t>(counter.QuadPart);
    // Split the conversion so a long uptime cannot overflow the multiply.
    return (ticks / frequency) * 1'000'000ULL
        + ((ticks % frequency) * 1'000'000ULL) / frequency;
}

[[nodiscard]] std::uint64_t systemTimeMilliseconds() noexcept {
    const auto value = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0U;
}

// Hands the receiver ownership while keeping an observer for telemetry. The
// receiver never replaces its decoder, so the pointer stays valid for as long
// as the receiver does.
[[nodiscard]] std::unique_ptr<IAudioPacketDecoder> makeStreamDecoder(
    JamLinkStreamDecoder*& observer) {
    auto decoder = std::make_unique<JamLinkStreamDecoder>(
        networkSampleRate, networkPacketFrames);
    observer = decoder.get();
    return decoder;
}

[[nodiscard]] AudioStreamReceiverSettings receiverSettings() noexcept {
    AudioStreamReceiverSettings settings;
    settings.sampleRate = networkSampleRate;
    settings.packetFrames = networkPacketFrames;
    settings.slotCount = 128U;
    // Two packets is 10 ms, which is the floor a musician should ever pay for
    // buffering; 32 packets caps it at 160 ms, past which playing together is
    // no longer realistic and the user needs to be told rather than buffered.
    settings.minimumDepthPackets = 2U;
    settings.maximumDepthPackets = 32U;
    return settings;
}

// The room's own keys, one per direction.
//
// These seal the two things that happen before a pair exists: the join
// request, whose sender has not heard from the other end and cannot know its
// nonce prefix, and the refusal that answers one when the room is full.
// Everything after the handshake is sealed with the pair key instead.
//
// Held per direction rather than per slot because there is nothing per-slot
// about them -- a musician hosting three pairs opens all three with the same
// guest-to-host room key. What keeps their packets apart is the nonce prefix,
// which every slot draws separately.
struct RoomCiphers final {
    AesGcmCipher hostToGuest;
    AesGcmCipher guestToHost;

    [[nodiscard]] AesGcmCipher& forDirection(Direction direction) noexcept {
        return direction == Direction::HostToGuest ? hostToGuest : guestToHost;
    }
    [[nodiscard]] bool valid() const noexcept {
        return hostToGuest.valid() && guestToHost.valid();
    }
};

// Everything JamLink holds about one other musician.
//
// This was a single set of members on the transport, which is exactly the right
// shape for a duo and exactly the shape a mesh has to grow out of. Gathering it
// here while there is still only ever one of them keeps the change that
// introduces a second peer from also being the change that has to discover
// which state was per-peer all along -- and that discovery, made in the middle
// of a working transport, is where the subtle faults would come from.
//
// Deliberately not everything. The socket, the capture rings, the send pacers,
// the send limiters and the local mute state belong to this machine rather than
// to any one peer: a mesh encodes a packet once and seals it separately for
// each recipient, so anything on the near side of the encoder stays shared.
//
// The room secret is still shared too. Per-peer key material is the next step
// and has to be, because reusing a nonce across two peers under one key would
// be a serious defect rather than an optimisation.
struct PeerSlot final {
    PeerSlot()
        : receivers{
              AudioStreamReceiver(
                  receiverSettings(), makeStreamDecoder(streamDecoders[0])),
              AudioStreamReceiver(
                  receiverSettings(), makeStreamDecoder(streamDecoders[1]))} {}

    // Declared before the receivers so it is initialised first; their
    // constructor fills it in.
    std::array<JamLinkStreamDecoder*, audioStreamCount> streamDecoders{};
    std::array<AudioStreamReceiver, audioStreamCount> receivers;
    std::array<audio::GainStage, audioStreamCount> remoteGain{
        audio::GainStage(1.0F), audio::GainStage(1.0F)};
    std::array<audio::RealtimeAtomicFloat, audioStreamCount> remotePeak{
        audio::RealtimeAtomicFloat(0.0F), audio::RealtimeAtomicFloat(0.0F)};

    // Whether anybody holds this slot at all.
    //
    // Distinct from `connected` on purpose. A peer that times out keeps its
    // slot and its addresses, because those are what let a dropped session
    // re-form without a fresh invite; freeing them the moment somebody went
    // quiet would throw away the endpoint the reconnect needs. The slot is
    // released only when another musician actually needs one.
    bool inUse{false};
    // Which end of this pair we are. Room-wide host mode answers this for a
    // duo; a mesh has to answer it pair by pair, because a musician can be
    // the host of one pair and the guest of another in the same room.
    bool hostRole{true};

    // Where this peer is, and how packets to it are sealed and checked.
    sockaddr_in remoteAddress{};
    std::atomic<bool> remoteEndpointKnown{false};
    Direction sendDirection{Direction::HostToGuest};
    Direction receiveDirection{Direction::GuestToHost};
    ReplayWindow replayWindow;
    std::array<std::uint8_t, noncePrefixBytes> noncePrefix{};
    // The other end's prefix, read from the header of anything they send. It
    // is in the clear because the receiver needs it to rebuild the nonce, and
    // that is exactly what makes a per-pair key derivable without a round trip.
    std::array<std::uint8_t, noncePrefixBytes> remoteNoncePrefix{};
    bool remotePrefixKnown{false};
    // Whether this peer is in session, and the clocks that decide when to
    // give up on it or prod it.
    //
    // These were locals in the worker, which is correct while there is one
    // peer and wrong the moment there are two: a single connected flag cannot
    // describe a room where one musician is present and another has dropped,
    // and a single receive deadline would time the whole room out on the
    // silence of whichever peer left.
    bool connected{false};
    bool publishedCandidates{false};
    ULONGLONG lastReceive{0U};
    ULONGLONG lastHello{0U};
    ULONGLONG lastPing{0U};
    // What this link had accepted and concealed at the last control packet,
    // so the next one reports the window rather than the lifetime. Per peer,
    // because the far end sets its send rate from this and a room-wide figure
    // would have everybody step down for one person's bad connection.
    std::array<std::uint64_t, audioStreamCount> lastAccepted{};
    std::array<std::uint64_t, audioStreamCount> lastConcealed{};

    // Keys belonging to this pair rather than to the room. Absent until the
    // other end has said something, which is why the join request itself is
    // still sealed with the room key and nothing else is.
    std::optional<AesGcmCipher> pairSend;
    std::optional<AesGcmCipher> pairReceive;
    std::uint32_t nonceCounter{0U};
    bool nonceExhausted{false};
    std::array<std::uint32_t, audioStreamCount> sendSequence{};

    // Who they are, and what they say about what they are sending.
    PeerParticipantInfo remoteParticipant;
    std::array<std::atomic<std::uint32_t>, audioStreamCount> remoteSourceClipped{};
    // What this peer says they are deliberately not sending.
    std::array<std::atomic<std::uint32_t>, audioStreamCount> remoteStreamMutedByPeer{};
    std::atomic<std::uint64_t> sessionsEstablished{0U};

    // Measured against this peer alone. One number for the room would be a
    // different musician's link every time it moved.
    std::atomic<std::uint64_t> roundTripMicroseconds{0U};
    std::atomic<bool> roundTripMeasured{false};

    // Touched only by the network worker; the rate it decides crosses to the
    // send loop through the atomics beside it.
    std::array<BitrateController, audioStreamCount> bitrateControllers{};
    std::array<std::atomic<std::uint32_t>, audioStreamCount> pendingBitrate{};
    // What this peer has most recently asked for, kept rather than consumed.
    //
    // The encoder is shared -- a mesh encodes once and seals per recipient --
    // so it can only run at one rate, and that rate has to be the lowest
    // anybody asked for. Encoding per peer instead would multiply the most
    // expensive thing the send path does by the size of the room to spare the
    // healthy links a smaller packet, which is the wrong trade. A one-shot
    // exchange() could not do this: the minimum has to be recomputed from
    // every peer's standing request each pass, not from whoever spoke last.
    std::array<std::uint32_t, audioStreamCount> requestedBitrate{};

    // Path finding to this peer. Written before the worker starts and
    // afterwards touched only by the worker thread.
    std::vector<IceCandidate> remoteCandidates;
    IceAgent ice;
};


class WindowsPeerAudioTransport final : public IPeerAudioTransport {
public:
    WindowsPeerAudioTransport()
        : localAudio_{
              audio::SpscAudioRing(32'768U, 1U),
              audio::SpscAudioRing(32'768U, 1U)} {
        // One slot today. Everything that follows addresses it by index rather
        // than by being the only thing there is.
        peers_[0] = std::make_unique<PeerSlot>();
        // Core derives the pair keys and holds no crypto of its own, so the
        // platform primitive is handed to it once here. Without it derivation
        // fails closed rather than falling back to anything weaker.
        installHmacSha256(&hmacSha256);
    }

    [[nodiscard]] PeerSlot& peer(std::size_t index = 0U) noexcept {
        return *peers_[index];
    }
    [[nodiscard]] const PeerSlot& peer(std::size_t index = 0U) const noexcept {
        return *peers_[index];
    }

    // Every slot somebody holds, connected or merely remembered. Written as a
    // callback rather than an iterator so that the null slots -- the ones
    // nobody has ever taken -- cannot be reached by accident.
    // Not noexcept: the worker's per-slot work builds strings and control
    // events, and run() catches. A noexcept boundary here would turn an
    // allocation failure into a terminate instead of the SocketFailed the
    // rest of the transport reports.
    template <typename Visitor>
    void forEachSlot(Visitor&& visit) {
        for (auto& held : peers_) {
            if (held != nullptr && held->inUse) {
                visit(*held);
            }
        }
    }
    template <typename Visitor>
    void forEachSlot(Visitor&& visit) const noexcept {
        for (const auto& held : peers_) {
            if (held != nullptr && held->inUse) {
                visit(*held);
            }
        }
    }

    // Whether a packet was sealed by this machine. Every prefix this end
    // sends under is drawn from the system generator, so a match is proof
    // rather than a heuristic.
    [[nodiscard]] bool isOwnNoncePrefix(
        std::span<const std::uint8_t, noncePrefixBytes> prefix) const noexcept {
        if (std::memcmp(prefix.data(), refusalNoncePrefix_.data(), noncePrefixBytes) == 0) {
            return true;
        }
        for (const auto& held : peers_) {
            if (held != nullptr && held->inUse
                && std::memcmp(
                       prefix.data(), held->noncePrefix.data(), noncePrefixBytes) == 0) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] std::size_t slotIndexOf(const PeerSlot& slot) const noexcept {
        for (std::size_t index = 0U; index < maximumPeerSlots; ++index) {
            if (peers_[index].get() == &slot) {
                return index;
            }
        }
        return 0U;
    }

    // Who is in session right now, one bit per slot. Taken as a snapshot
    // because anything that fans out has to agree with itself about the room
    // for the length of one pass.
    [[nodiscard]] std::uint32_t connectedSlotMask() const noexcept {
        std::uint32_t mask = 0U;
        for (std::size_t index = 0U; index < maximumPeerSlots; ++index) {
            const PeerSlot* slot = peers_[index].get();
            if (slot != nullptr && slot->inUse && slot->connected) {
                mask |= 1U << index;
            }
        }
        return mask;
    }

    [[nodiscard]] PeerParticipantInfo slotParticipant(const PeerSlot& slot) const {
        const std::scoped_lock lock(controlMutex_);
        return slot.remoteParticipant;
    }

    [[nodiscard]] static std::uint64_t chatKey(
        const std::string& profileId, std::uint64_t messageId) noexcept {
        std::uint64_t hash = 0xCBF29CE484222325ULL;
        for (const char character : profileId) {
            hash ^= static_cast<std::uint8_t>(character);
            hash *= 0x100000001B3ULL;
        }
        return hash ^ (messageId * 0x9E3779B97F4A7C15ULL);
    }

    [[nodiscard]] std::size_t connectedPeerCount() const noexcept {
        std::size_t count = 0U;
        forEachSlot([&count](const PeerSlot& slot) {
            if (slot.connected) {
                ++count;
            }
        });
        return count;
    }

    // The first musician in session. A duo has exactly one and every part of
    // the interface that shows "your friend" means this one.
    [[nodiscard]] const PeerSlot* firstConnectedSlot() const noexcept {
        for (const auto& held : peers_) {
            if (held != nullptr && held->inUse && held->connected) {
                return held.get();
            }
        }
        return nullptr;
    }

    // Which slot a datagram belongs to.
    //
    // Source address against live slots first, and only then anything else.
    // The other way round -- deciding from what the packet claims and
    // checking the address afterwards -- would let anyone take a slot by
    // sending anything.
    [[nodiscard]] PeerSlot* slotForPacket(
        const sockaddr_in& source, PacketType type) noexcept {
        for (auto& held : peers_) {
            if (held != nullptr && held->inUse && held->remoteAddress.sin_port != 0U
                && sameEndpoint(source, held->remoteAddress)) {
                return held.get();
            }
        }
        // While this end is still finding a path, the answer legitimately
        // arrives from an address it never probed: a router may rewrite the
        // source and several candidates are in flight at once. Only the
        // replies that can come before a session exists are let through this
        // way, and only to a slot that is still reaching out -- everything
        // after the handshake is held to the settled endpoint.
        if (type != PacketType::HelloAck && type != PacketType::RoomFull) {
            return nullptr;
        }
        for (auto& held : peers_) {
            if (held != nullptr && held->inUse && !held->connected && !held->hostRole) {
                return held.get();
            }
        }
        return nullptr;
    }

    // Somebody already here under that name. Two musicians cannot hold two
    // slots under one identity: a copied, self-asserted profile ID would
    // otherwise buy a second seat in the room beside the person it was copied
    // from.
    [[nodiscard]] PeerSlot* slotForParticipant(const std::string& profileId) noexcept {
        if (profileId.empty()) {
            return nullptr;
        }
        for (auto& held : peers_) {
            if (held != nullptr && held->inUse
                && held->remoteParticipant.profileId == profileId) {
                return held.get();
            }
        }
        return nullptr;
    }

    // A free slot, or the one belonging to whoever has been gone longest.
    //
    // Recycling rather than refusing is the point of keeping a timed-out
    // peer's addresses: they are worth holding until somebody actually needs
    // the room, and no longer.
    [[nodiscard]] PeerSlot* allocateSlot() {
        for (auto& held : peers_) {
            if (held == nullptr) {
                held = std::make_unique<PeerSlot>();
                return held.get();
            }
            if (!held->inUse) {
                return held.get();
            }
        }
        PeerSlot* stalest = nullptr;
        for (auto& held : peers_) {
            if (held->connected) {
                continue;
            }
            if (stalest == nullptr || held->lastReceive < stalest->lastReceive) {
                stalest = held.get();
            }
        }
        return stalest;
    }

    // Puts a slot into the state a fresh session starts from.
    //
    // Run when a slot is handed to a musician -- a join, an admission, or a
    // recycle -- rather than once for the whole transport, because a room
    // does not start its sessions at the same moment. The nonce prefix is
    // drawn here and not shared: two slots sealing under the room key with
    // one prefix would reuse a nonce across two packets under one key, which
    // is a break rather than an inefficiency.
    [[nodiscard]] bool prepareSlotSession(PeerSlot& slot, bool hostRole) {
        slot.inUse = true;
        slot.hostRole = hostRole;
        slot.sendDirection = hostRole ? Direction::HostToGuest : Direction::GuestToHost;
        slot.receiveDirection = hostRole ? Direction::GuestToHost : Direction::HostToGuest;
        slot.connected = false;
        slot.publishedCandidates = false;
        slot.replayWindow.reset();
        slot.nonceCounter = 0U;
        slot.nonceExhausted = false;
        // Cleared with the rest of the session state. A stale pair key would
        // otherwise outlive the session it belonged to and be used to seal
        // the next one.
        slot.pairSend.reset();
        slot.pairReceive.reset();
        slot.remotePrefixKnown = false;
        {
            // Read by the interface thread, so cleared under the same lock
            // that publishes it rather than torn between the two.
            const std::scoped_lock lock(controlMutex_);
            slot.remoteParticipant = {};
        }
        slot.lastReceive = GetTickCount64();
        slot.lastHello = 0U;
        slot.lastPing = 0U;
        slot.roundTripMicroseconds.store(0U, std::memory_order_relaxed);
        slot.roundTripMeasured.store(false, std::memory_order_relaxed);
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            slot.receivers[index].reset();
            slot.receivers[index].configureDepth(
                latencyMinimumPackets_, latencyMaximumPackets_, latencySafety_);
            slot.remoteGain[index].setLinearGain(remoteGain_[index]);
            slot.remoteGain[index].setMuted(remoteMuted_[index]);
            slot.sendSequence[index] = 0U;
            slot.remotePeak[index].store(0.0F);
            slot.remoteStreamMutedByPeer[index].store(0U, std::memory_order_release);
            slot.remoteSourceClipped[index].store(0U, std::memory_order_relaxed);
            slot.bitrateControllers[index].reset();
            slot.pendingBitrate[index].store(0U, std::memory_order_relaxed);
            slot.requestedBitrate[index] = 0U;
            slot.lastAccepted[index] = 0U;
            slot.lastConcealed[index] = 0U;
        }
        return BCRYPT_SUCCESS(BCryptGenRandom(
            nullptr, slot.noncePrefix.data(),
            static_cast<ULONG>(slot.noncePrefix.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG));
    }

    // Whether the room can carry one more, asked of the capacity guard rather
    // than answered again here. It already knows that measured strain
    // outranks arithmetic and that six is the ceiling; a second rule invented
    // at this call site could only disagree with it.
    [[nodiscard]] bool canAdmitAnotherMusician() const noexcept {
        jamlink::control::RoomCapacityEvidence evidence;
        // Everyone in the room including this machine.
        evidence.participants = static_cast<std::uint32_t>(connectedPeerCount() + 1U);
        evidence.streamsPerParticipant =
            static_cast<std::uint32_t>(audioStreamCount);
        std::uint32_t lowestRate = outgoingBitsPerSecond;
        bool reduced = false;
        forEachSlot([&lowestRate, &reduced](const PeerSlot& slot) {
            for (const auto& controller : slot.bitrateControllers) {
                lowestRate = std::min(lowestRate, controller.bitsPerSecond());
                reduced = reduced || controller.reductions() > 0U;
            }
        });
        // The rate actually in use, not the one the build starts at. A link
        // that has already stepped down has more room than the nominal figure
        // suggests, and this and the bitrate controller must not contradict
        // each other.
        evidence.bitsPerSecondPerStream = lowestRate;
        evidence.sendRateAlreadyReduced = reduced;
        return jamlink::control::evaluateRoomCapacity(evidence).canAdmitAnother;
    }
    ~WindowsPeerAudioTransport() override { stop(); }

    [[nodiscard]] std::string host(
        std::uint16_t port,
        bool discoverPublicAddress,
        bool requestAutomaticPortMapping) override {
        stop();
        if (!winsock_.available() || !createBoundSocket(port)) {
            state_.store(PeerConnectionState::SocketFailed, std::memory_order_release);
            return {};
        }
        udpBound_.store(true, std::memory_order_release);
        if (!BCRYPT_SUCCESS(BCryptGenRandom(
                nullptr, secret_.data(), static_cast<ULONG>(secret_.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
            socket_.reset();
            localPort_ = 0U;
            udpBound_.store(false, std::memory_order_release);
            return {};
        }

        const std::string localAddress = localIpv4Address();
        const bool mappingRequested = discoverPublicAddress
            && requestAutomaticPortMapping;
        PortMappingResult mapping = mappingRequested
            ? addAutomaticPortMapping(localPort_, localAddress)
            : PortMappingResult{};
        // UPnP is the most common of the three but is switched off by default
        // on plenty of routers, and some ship a broken implementation while
        // supporting PCP or NAT-PMP perfectly well. With a single invite code
        // the host cannot learn the guest's endpoint first, so a working port
        // mapping is the only thing that makes it reachable at all.
        JAMLINK_LOG("host", "bound UDP port " + std::to_string(localPort_)
            + ", local address " + localAddress
            + ", gateway " + (defaultGatewayAddress().empty()
                ? std::string("not found") : defaultGatewayAddress()));
        if (mappingRequested && !mapping.mapped) {
            JAMLINK_LOG("host", "UPnP mapping refused, trying PCP and NAT-PMP");
            const GatewayMapping gateway = requestGatewayMapping(
                localPort_, localAddress, portMappingLifetimeSeconds);
            if (gateway.mapped && gateway.externalPort == localPort_) {
                mapping.mapped = true;
                mapping.externalAddress = gateway.externalAddress;
                gatewayMappingProtocol_ = gateway.protocol;
                JAMLINK_LOG("host", std::string("port mapped by ") + gateway.protocol);
            } else if (gateway.mapped) {
                // The router granted a different external port. The invite
                // carries the internal port, so a rewritten port would send the
                // guest somewhere that is not us.
                gatewayMappingProtocol_ = "";
                JAMLINK_LOG("host", std::string("router granted external port ")
                    + std::to_string(gateway.externalPort) + " instead of "
                    + std::to_string(localPort_) + ", unusable for a direct invite");
            } else {
                JAMLINK_LOG("host", "no router mapping from UPnP, PCP, or NAT-PMP");
            }
        } else if (mapping.mapped) {
            gatewayMappingProtocol_ = "UPnP";
            JAMLINK_LOG("host", "port mapped by UPnP");
        }
        mappedPort_ = mapping.mapped;
        portMapping_.store(
            mappingRequested
                ? (mapping.mapped ? PortMappingState::Succeeded : PortMappingState::Failed)
                : PortMappingState::NotRequested,
            std::memory_order_release);
        const StunEndpoint publicEndpoint = discoverPublicAddress
            ? queryStun(socket_.get())
            : StunEndpoint{};
        publicAddressDiscovery_.store(
            discoverPublicAddress
                ? (publicEndpoint.succeeded
                    ? PublicAddressDiscoveryState::Succeeded
                    : PublicAddressDiscoveryState::Failed)
                : PublicAddressDiscoveryState::NotAttempted,
            std::memory_order_release);
        JAMLINK_LOG("host", publicEndpoint.succeeded
            ? "STUN reported public endpoint " + publicEndpoint.address + ":"
                + std::to_string(publicEndpoint.port)
                + (publicEndpoint.port == localPort_
                    ? " (port preserved)"
                    : " (port rewritten from " + std::to_string(localPort_)
                        + ", which means this router is symmetric and a direct"
                          " invite cannot work)")
            : std::string("STUN public address discovery failed"));
        // Measured only when the host is trying to be reachable. A second
        // probe costs a second of startup and answers nothing offline.
        const NatAssessment nat = discoverPublicAddress
            ? probeNatBehaviour(socket_.get(), publicEndpoint)
            : NatAssessment{};
        if (publicEndpoint.succeeded) {
            publicAddress_ = publicEndpoint.address;
            publicPort_ = publicEndpoint.port;
        }
        natBehaviour_.store(nat.behaviour, std::memory_order_release);
        JAMLINK_LOG("host", std::string("router mapping behaviour ")
            + std::string(natBehaviourName(nat.behaviour))
            + (nat.canHostDirectly()
                ? ""
                : ", so an invite made here names an endpoint nobody can reach"));
        const bool usableAutomaticMapping = mapping.mapped
            && (publicEndpoint.succeeded || !mapping.externalAddress.empty());
        automaticPortMapping_.store(usableAutomaticMapping, std::memory_order_relaxed);
        // A router that hands out a fresh port per destination outranks a
        // granted mapping: the mapping is real and the invite still leads
        // nowhere, which is exactly the combination that produced a discovered
        // public address, a correct-looking invite, and no connection.
        reachability_.store(
            !nat.canHostDirectly()
                ? ReachabilityAssessment::RelayRequired
                : (usableAutomaticMapping
                    ? ReachabilityAssessment::LikelyReachable
                    : ReachabilityAssessment::Unknown),
            std::memory_order_release);
        // Every address this machine could be reached on, rather than one
        // guess. The LAN address is what wins when two musicians are in the
        // same building, and dropping it -- which naming only the public
        // address did -- meant that pair had to hairpin back through a router
        // that often will not do it at all.
        std::vector<IceCandidate> candidates;
        const auto offer = [&candidates](
            const std::string& address, std::uint16_t port, CandidateKind kind) {
            if (address.empty() || port == 0U) {
                return;
            }
            for (const auto& existing : candidates) {
                if (existing.address == address && existing.port == port) {
                    return;
                }
            }
            candidates.push_back(IceCandidate{address, port, kind});
        };
        if (publicEndpoint.succeeded) {
            // The port STUN saw, not the one we bound. On a symmetric router
            // those differ, and the bound port is then simply the wrong answer.
            offer(publicEndpoint.address, publicEndpoint.port,
                CandidateKind::ServerReflexive);
        }
        // UPnP requests the same external and internal UDP port, so keeping
        // that port here also makes manual forwarding explicit and
        // deterministic when automatic mapping is unavailable.
        offer(mapping.externalAddress, localPort_, CandidateKind::ServerReflexive);
        offer(localAddress, localPort_, CandidateKind::Host);
        inviteCode_ = "JL2|" + encodeCandidates(candidates) + "|" + hexEncode(secret_);
        JAMLINK_LOG("host", "offering " + std::to_string(candidates.size())
            + " candidate address(es)");
        hostMode_ = true;
        JAMLINK_LOG("host", "waiting for a guest, invite "
            + jamlink::diagnostics::SessionLog::redactInvite(inviteCode_));
        state_.store(PeerConnectionState::WaitingForPeer, std::memory_order_release);
        launchWorker();
        return inviteCode_;
    }

    [[nodiscard]] bool join(const std::string& inviteCode) override {
        stop();
        // The invite names one musician, so this end takes one slot and
        // reaches out on it as that pair's guest. Every other slot stays free
        // for whoever the room introduces later.
        if (!prepareSlotSession(peer(), false)) {
            state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
            return false;
        }
        peer().remoteCandidates.clear();
        if (!parseInviteCandidates(inviteCode, peer().remoteCandidates, secret_)) {
            state_.store(PeerConnectionState::InviteInvalid, std::memory_order_release);
            return false;
        }
        const std::string address = peer().remoteCandidates.front().address;
        const std::uint16_t port = peer().remoteCandidates.front().port;
        if (!winsock_.available() || !createBoundSocket(0U)) {
            state_.store(PeerConnectionState::SocketFailed, std::memory_order_release);
            return false;
        }
        udpBound_.store(true, std::memory_order_release);
        JAMLINK_LOG("join", "joining " + std::to_string(peer().remoteCandidates.size())
            + " candidate address(es), first " + address + ":" + std::to_string(port)
            + ", from local port " + std::to_string(localPort_));
        peer().remoteEndpointKnown.store(true, std::memory_order_release);
        beginCandidateChecks(peer(), GetTickCount64() * 1'000ULL);
        peer().remoteAddress = {};
        peer().remoteAddress.sin_family = AF_INET;
        peer().remoteAddress.sin_port = htons(port);
        if (inet_pton(AF_INET, address.c_str(), &peer().remoteAddress.sin_addr) != 1) {
            state_.store(PeerConnectionState::InviteInvalid, std::memory_order_release);
            socket_.reset();
            localPort_ = 0U;
            udpBound_.store(false, std::memory_order_release);
            return false;
        }
        inviteCode_ = inviteCode;
        hostMode_ = false;
        state_.store(PeerConnectionState::Connecting, std::memory_order_release);
        launchWorker();
        return true;
    }

    void stop() noexcept override {
        stopRequested_.store(true, std::memory_order_release);
        if (worker_.joinable()) {
            worker_.join();
        }
        socket_.reset();
        if (mappedPort_) {
            removeAutomaticPortMapping(localPort_);
        }
        mappedPort_ = false;
        localPort_ = 0U;
        inviteCode_.clear();
        automaticPortMapping_.store(false, std::memory_order_relaxed);
        udpBound_.store(false, std::memory_order_release);
        publicAddressDiscovery_.store(
            PublicAddressDiscoveryState::NotAttempted, std::memory_order_release);
        portMapping_.store(PortMappingState::NotRequested, std::memory_order_release);
        reachability_.store(ReachabilityAssessment::Unknown, std::memory_order_release);
        {
            const std::scoped_lock lock(controlMutex_);
            pendingChat_.clear();
            controlEvents_.clear();
            outboundChatTimes_.clear();
            for (auto& held : peers_) {
                if (held != nullptr) {
                    held->remoteParticipant = {};
                }
            }
        }
        {
            const std::scoped_lock lock(rosterMutex_);
            roster_.clear();
            rosterSize_.store(0U, std::memory_order_release);
        }
        receivedChatIds_.fill(0U);
        receivedChatCount_ = 0U;
        receivedChatCursor_ = 0U;
        inboundChatTimes_.clear();
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            localAudio_[index].clear();
        }
        // Every slot, and every slot released. A room that has been left owns
        // nobody: keeping a timed-out peer's endpoint is worth doing while the
        // session is running and is only stale state once it is not.
        for (auto& held : peers_) {
            if (held == nullptr) {
                continue;
            }
            held->inUse = false;
            held->connected = false;
            held->remoteEndpointKnown.store(false, std::memory_order_release);
            held->remoteAddress = {};
            held->pairSend.reset();
            held->pairReceive.reset();
            held->remotePrefixKnown = false;
            for (std::size_t index = 0U; index < audioStreamCount; ++index) {
                held->receivers[index].reset();
                held->remotePeak[index].store(0.0F);
                held->sendSequence[index] = 0U;
            }
        }
        state_.store(PeerConnectionState::Idle, std::memory_order_release);
    }

    void setLocalParticipant(PeerParticipantInfo participant) override {
        if (state_.load(std::memory_order_acquire) != PeerConnectionState::Idle
            || !validParticipant(participant)) {
            return;
        }
        const std::scoped_lock lock(controlMutex_);
        localParticipant_ = std::move(participant);
    }

    [[nodiscard]] bool sendChatMessage(const std::string& plainText) override {
        if (state_.load(std::memory_order_acquire) != PeerConnectionState::Connected
            || plainText.empty() || plainText.size() > maximumChatMessageBytes
            || !validUtf8(plainText, true)) {
            return false;
        }
        const ULONGLONG now = GetTickCount64();
        const std::scoped_lock lock(controlMutex_);
        while (!outboundChatTimes_.empty()
               && now - outboundChatTimes_.front() >= chatRateWindowMilliseconds) {
            outboundChatTimes_.pop_front();
        }
        if (outboundChatTimes_.size() >= maximumChatMessagesPerWindow
            || pendingChat_.size() >= maximumPendingChatMessages) {
            return false;
        }
        outboundChatTimes_.push_back(now);
        pendingChat_.push_back(PendingChat{
            nextChatMessageId_++, systemTimeMilliseconds(), plainText, 0U, 0U});
        return true;
    }

    [[nodiscard]] std::vector<RoomControlEvent> takeControlEvents() override {
        std::vector<RoomControlEvent> result;
        const std::scoped_lock lock(controlMutex_);
        result.reserve(controlEvents_.size());
        while (!controlEvents_.empty()) {
            result.push_back(std::move(controlEvents_.front()));
            controlEvents_.pop_front();
        }
        return result;
    }

    [[nodiscard]] PeerParticipantInfo remoteParticipant() const override {
        const std::scoped_lock lock(controlMutex_);
        // The first musician in session rather than whoever happens to hold
        // slot zero: after a peer drops and somebody else takes their slot,
        // slot zero can be an empty seat while the room is full of people.
        const PeerSlot* connected = firstConnectedSlot();
        return connected != nullptr
            ? connected->remoteParticipant : peer().remoteParticipant;
    }

    [[nodiscard]] std::vector<PeerParticipantInfo> peerParticipants() const override {
        const std::scoped_lock lock(controlMutex_);
        std::vector<PeerParticipantInfo> result(maximumPeerSlots);
        for (std::size_t index = 0U; index < maximumPeerSlots; ++index) {
            const PeerSlot* slot = peers_[index].get();
            if (slot != nullptr && slot->inUse) {
                result[index] = slot->remoteParticipant;
            }
        }
        return result;
    }

    void setSendMuted(bool muted) noexcept override {
        sendMuted_.store(muted ? 1U : 0U, std::memory_order_release);
    }

    void setLocalStreamMuted(AudioStreamId stream, bool muted) noexcept override {
        localStreamMuted_[streamIndex(stream)].store(
            muted ? 1U : 0U, std::memory_order_release);
    }

    void setLocalStreamClipState(AudioStreamId stream, bool clipped) noexcept override {
        localSourceClipped_[streamIndex(stream)].store(
            clipped ? 1U : 0U, std::memory_order_release);
    }

    // Applied to everybody, because the interface has one level per stream
    // rather than one per musician. Per-participant level and mute are the
    // next step and are what turn this from a convenience into how somebody
    // copes with one person's bad connection; until then, meaning "everyone"
    // is at least the same thing this control has always meant.
    void setRemoteStreamGain(AudioStreamId stream, float gain) noexcept override {
        const std::size_t index = streamIndex(stream);
        remoteGain_[index] = gain;
        for (auto& held : peers_) {
            if (held != nullptr) {
                held->remoteGain[index].setLinearGain(gain);
            }
        }
    }

    void setRemoteStreamMuted(AudioStreamId stream, bool muted) noexcept override {
        const std::size_t index = streamIndex(stream);
        remoteMuted_[index] = muted;
        for (auto& held : peers_) {
            if (held != nullptr) {
                held->remoteGain[index].setMuted(muted);
            }
        }
    }

    void setLatencyPreference(LatencyPreference preference) noexcept override {
        // Minimum depth sets the floor a musician always pays; the maximum is
        // the point past which playing together is no longer realistic and the
        // connection grade should be saying so instead.
        std::size_t minimumPackets = 2U;
        std::size_t maximumPackets = 32U;
        double safety = 2.5;
        if (preference == LatencyPreference::Lowest) {
            minimumPackets = 1U;
            maximumPackets = 12U;
            safety = 1.75;
        } else if (preference == LatencyPreference::MostStable) {
            minimumPackets = 4U;
            maximumPackets = 48U;
            safety = 3.5;
        }
        latencyMinimumPackets_ = minimumPackets;
        latencyMaximumPackets_ = maximumPackets;
        latencySafety_ = safety;
        for (auto& held : peers_) {
            if (held == nullptr) {
                continue;
            }
            for (auto& receiver : held->receivers) {
                receiver.configureDepth(minimumPackets, maximumPackets, safety);
            }
        }
    }

    [[nodiscard]] std::string inviteCode() const override { return inviteCode_; }
    [[nodiscard]] std::uint16_t localPort() const noexcept override { return localPort_; }

    [[nodiscard]] std::vector<RosterMember> roomMembers() const override {
        const std::scoped_lock lock(rosterMutex_);
        return roster_.members();
    }

    [[nodiscard]] PeerTransportTelemetry telemetry() const noexcept override {
        PeerTransportTelemetry snapshot;
        snapshot.state = state_.load(std::memory_order_acquire);
        snapshot.packetsSent = packetsSent_.load(std::memory_order_relaxed);
        snapshot.packetsReceived = packetsReceived_.load(std::memory_order_relaxed);
        snapshot.packetsRejected = packetsRejected_.load(std::memory_order_relaxed);
        // The first musician in session, because that is what everything
        // showing one friend already meant by these. A room reads
        // snapshot.peers instead, which keeps them apart.
        const PeerSlot* primary = firstConnectedSlot();
        if (primary == nullptr) {
            primary = peers_[0].get();
        }
        snapshot.roundTripMicroseconds =
            primary->roundTripMicroseconds.load(std::memory_order_relaxed);
        snapshot.roundTripMeasured =
            primary->roundTripMeasured.load(std::memory_order_relaxed);
        snapshot.portMappingProtocol = gatewayMappingProtocol_;
        snapshot.automaticPortMapping =
            automaticPortMapping_.load(std::memory_order_relaxed);
        snapshot.udpBound = udpBound_.load(std::memory_order_acquire);
        snapshot.publicAddressDiscovery =
            publicAddressDiscovery_.load(std::memory_order_acquire);
        snapshot.portMapping = portMapping_.load(std::memory_order_acquire);
        snapshot.reachability = reachability_.load(std::memory_order_acquire);
        snapshot.encodeFailures = encodeFailures_.load(std::memory_order_relaxed);
        snapshot.sessionsEstablished =
            primary->sessionsEstablished.load(std::memory_order_relaxed);
        snapshot.natBehaviour = natBehaviour_.load(std::memory_order_acquire);
        for (const auto& limiter : sendLimiters_) {
            snapshot.limitedSendSamples += limiter.limitedSamples();
        }
        snapshot.candidateProbesSent = iceProbes_.load(std::memory_order_relaxed);
        snapshot.candidateRoundsExhausted = iceRounds_.load(std::memory_order_relaxed);
        // What is actually being sent now, not the rate the build starts
        // at. Reporting the constant while the link had quietly halved it
        // is the same class of untruth as the fields this bundle already
        // had to have fixed.
        snapshot.audioBitsPerSecond = primary->bitrateControllers[0].bitsPerSecond();
        snapshot.bitrateReductions = primary->bitrateControllers[0].reductions();
        snapshot.uplinkExhausted = primary->bitrateControllers[0].exhausted();
        // Codec health is a property of this build rather than of one link,
        // so it counts every decoder in the room.
        for (const auto& held : peers_) {
            if (held == nullptr) {
                continue;
            }
            for (const auto* decoder : held->streamDecoders) {
                if (decoder == nullptr) {
                    continue;
                }
                snapshot.opusPacketsDecoded += decoder->opusPacketsDecoded();
                snapshot.pcmPacketsDecoded += decoder->pcmPacketsDecoded();
                snapshot.undecodablePackets += decoder->unusablePackets();
            }
        }
        const auto fillStreams = [](
            const PeerSlot& slot,
            std::array<RemoteStreamTelemetry, audioStreamCount>& into) noexcept {
            for (std::size_t index = 0U; index < audioStreamCount; ++index) {
                const auto receiver = slot.receivers[index].telemetry();
                RemoteStreamTelemetry& stream = into[index];
                stream.peak = slot.remotePeak[index].load();
                stream.sourceClipped =
                    slot.remoteSourceClipped[index].load(std::memory_order_acquire) != 0U;
                stream.mutedByPeer =
                    slot.remoteStreamMutedByPeer[index].load(std::memory_order_acquire) != 0U;
                stream.packetsAccepted = receiver.packetsAccepted;
                stream.packetsConcealed = receiver.packetsConcealed;
                stream.packetsLate = receiver.packetsLate;
                stream.bufferStretches = receiver.bufferStretches;
                stream.latencyTrims = receiver.latencyTrims;
                stream.jitterMicroseconds = receiver.jitterMicroseconds;
                stream.bufferedFrames = receiver.currentDepthFrames;
                stream.targetFrames = receiver.targetDepthFrames;
                stream.playing = receiver.playing;
            }
        };
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            snapshot.localAudioDrops += localAudio_[index].overrunCount();
        }
        fillStreams(*primary, snapshot.streams);
        for (std::size_t index = 0U; index < maximumPeerSlots; ++index) {
            const PeerSlot* slot = peers_[index].get();
            if (slot == nullptr || !slot->inUse) {
                continue;
            }
            PeerLinkTelemetry& link = snapshot.peers[index];
            link.inUse = true;
            link.connected = slot->connected;
            link.roundTripMicroseconds =
                slot->roundTripMicroseconds.load(std::memory_order_relaxed);
            link.roundTripMeasured =
                slot->roundTripMeasured.load(std::memory_order_relaxed);
            link.audioBitsPerSecond = slot->bitrateControllers[0].bitsPerSecond();
            link.bitrateReductions = slot->bitrateControllers[0].reductions();
            fillStreams(*slot, link.streams);
        }
        snapshot.connectedPeers = static_cast<std::uint32_t>(connectedPeerCount());
        // Read from an atomic rather than by taking the roster lock: this
        // call is noexcept, and locking is the one thing in it that could
        // throw.
        snapshot.expectedPeers = rosterSize_.load(std::memory_order_acquire);
        // A room nobody has reported into is not complete, it is unknown --
        // and saying a duo that has not finished its handshake is "whole"
        // would be the one reading nobody could act on.
        snapshot.roomComplete = snapshot.expectedPeers > 0U
            && snapshot.connectedPeers >= snapshot.expectedPeers;
        return snapshot;
    }

    void pushLocalAudio(
        AudioStreamId stream,
        std::span<const float> monoSamples,
        std::uint32_t sampleRate) noexcept override {
        const std::size_t index = streamIndex(stream);
        if (index >= audioStreamCount
            || state_.load(std::memory_order_acquire) != PeerConnectionState::Connected
            || sendMuted_.load(std::memory_order_acquire) != 0U
            || localStreamMuted_[index].load(std::memory_order_acquire) != 0U
            || sampleRate < 8'000U || sampleRate > 384'000U) {
            return;
        }
        localSampleRate_[index].store(sampleRate, std::memory_order_relaxed);
        static_cast<void>(localAudio_[index].write(monoSamples));
    }

    [[nodiscard]] std::size_t pullRemote48k(
        AudioStreamId stream,
        std::span<float> monoSamples) noexcept override {
        const std::size_t index = streamIndex(stream);
        if (index >= audioStreamCount) {
            std::fill(monoSamples.begin(), monoSamples.end(), 0.0F);
            return 0U;
        }
        // Several musicians are several receive buffers, so playback mixes
        // them rather than pulling from one. Each peer's level, mute and meter
        // are applied to that peer's own contribution before it is added:
        // metering the mix instead would leave one bad connection showing on
        // everybody's meter and nowhere to turn it down.
        std::fill(monoSamples.begin(), monoSamples.end(), 0.0F);
        std::size_t live = 0U;
        for (auto& held : peers_) {
            if (held == nullptr || !held->inUse) {
                continue;
            }
            PeerSlot& slot = *held;
            float peak = 0.0F;
            std::size_t slotLive = 0U;
            // In fixed blocks, because this runs on the audio callback and
            // must not allocate. The scratch buffer belongs to this thread
            // alone; nothing else touches it.
            for (std::size_t offset = 0U; offset < monoSamples.size();
                 offset += mixScratch_.size()) {
                const std::size_t frames =
                    std::min(mixScratch_.size(), monoSamples.size() - offset);
                const auto block = std::span<float>(mixScratch_.data(), frames);
                slotLive += slot.receivers[index].pull(block);
                slot.remoteGain[index].process(
                    audio::InterleavedAudioBlock{block, 1U});
                for (std::size_t frame = 0U; frame < frames; ++frame) {
                    peak = std::max(peak, std::abs(block[frame]));
                    monoSamples[offset + frame] += block[frame];
                }
            }
            slot.remotePeak[index].store(std::clamp(peak, 0.0F, 1.0F));
            // The frames carrying live audio from anybody. A room is playing
            // if any of it is, and summing would overcount the same frames
            // once per musician.
            live = std::max(live, slotLive);
        }
        return live;
    }

private:
    struct PendingChat final {
        std::uint64_t messageId{0U};
        std::uint64_t timestampMilliseconds{0U};
        std::string text;
        ULONGLONG lastAttemptMilliseconds{0U};
        std::uint32_t attempts{0U};
        // Which slots still owe an acknowledgement, one bit each. Chat is
        // room-wide, so one message goes to everybody and is not done until
        // everybody has said so -- erasing it on the first acknowledgement
        // would drop it for every musician after the first.
        std::uint32_t awaitingSlots{0U};
        // Whether the room it is addressed to has been decided yet. Fixed on
        // the first send pass rather than at the queue, and never revisited:
        // somebody who joins afterwards was not there for it, and a backlog
        // landing in their first seconds in the room is a surprise rather
        // than a conversation.
        bool addressed{false};
    };

    static constexpr std::size_t maximumPendingChatMessages = 64U;
    static constexpr std::size_t maximumControlEvents = 128U;
    static constexpr std::size_t maximumRememberedChatIds = 128U;
    static constexpr std::size_t maximumChatMessagesPerWindow = 8U;
    static constexpr ULONGLONG chatRateWindowMilliseconds = 2'000U;
    static constexpr ULONGLONG chatRetryMilliseconds = 250U;
    static constexpr std::uint32_t maximumChatAttempts = 12U;

    [[nodiscard]] PeerParticipantInfo localParticipantSnapshot() const {
        const std::scoped_lock lock(controlMutex_);
        return localParticipant_;
    }

    void setRemoteParticipant(PeerSlot& slot, const PeerParticipantInfo& participant) {
        const std::scoped_lock lock(controlMutex_);
        slot.remoteParticipant = participant;
    }

    void appendControlEvent(RoomControlEvent event) {
        const std::scoped_lock lock(controlMutex_);
        if (controlEvents_.size() >= maximumControlEvents) {
            controlEvents_.pop_front();
        }
        controlEvents_.push_back(std::move(event));
    }

    [[nodiscard]] bool rememberChatMessage(std::uint64_t messageId) noexcept {
        const std::size_t count = std::min(receivedChatCount_, receivedChatIds_.size());
        for (std::size_t index = 0U; index < count; ++index) {
            if (receivedChatIds_[index] == messageId) {
                return false;
            }
        }
        receivedChatIds_[receivedChatCursor_] = messageId;
        receivedChatCursor_ = (receivedChatCursor_ + 1U) % receivedChatIds_.size();
        receivedChatCount_ = std::min(receivedChatCount_ + 1U, receivedChatIds_.size());
        return true;
    }

    [[nodiscard]] static char candidateKindLetter(CandidateKind kind) noexcept {
        switch (kind) {
        case CandidateKind::Host: return 'h';
        case CandidateKind::ServerReflexive: return 's';
        case CandidateKind::Relayed: return 'r';
        }
        return 'h';
    }

    [[nodiscard]] static bool candidateKindFromLetter(
        char letter, CandidateKind& kind) noexcept {
        switch (letter) {
        case 'h': kind = CandidateKind::Host; return true;
        case 's': kind = CandidateKind::ServerReflexive; return true;
        case 'r': kind = CandidateKind::Relayed; return true;
        default: return false;
        }
    }

    // A machine has more than one address, and which of them a pair of routers
    // will carry cannot be known in advance. An invite that names only one is
    // a guess: naming the public address alone strands two musicians sitting in
    // the same building, and naming the LAN address alone is useless between
    // two homes.
    [[nodiscard]] static std::string encodeCandidates(
        const std::vector<IceCandidate>& candidates) {
        std::string encoded;
        for (const auto& candidate : candidates) {
            if (!candidate.valid()) {
                continue;
            }
            if (!encoded.empty()) {
                encoded += ',';
            }
            encoded += candidateKindLetter(candidate.kind);
            encoded += '=';
            encoded += candidate.address;
            encoded += ':';
            encoded += std::to_string(candidate.port);
        }
        return encoded;
    }

    [[nodiscard]] static bool decodeCandidates(
        const std::string& encoded, std::vector<IceCandidate>& candidates) {
        std::size_t at = 0U;
        while (at <= encoded.size()) {
            const std::size_t end = encoded.find(',', at);
            const std::string item = encoded.substr(
                at, end == std::string::npos ? std::string::npos : end - at);
            if (item.size() < 4U || item[1] != '=') {
                return false;
            }
            IceCandidate candidate;
            if (!candidateKindFromLetter(item[0], candidate.kind)) {
                return false;
            }
            const std::size_t colon = item.rfind(':');
            if (colon == std::string::npos || colon < 3U) {
                return false;
            }
            candidate.address = item.substr(2U, colon - 2U);
            unsigned long parsed = 0UL;
            try {
                std::size_t consumed = 0U;
                parsed = std::stoul(item.substr(colon + 1U), &consumed);
                if (consumed != item.size() - colon - 1U) {
                    return false;
                }
            } catch (...) {
                return false;
            }
            if (parsed == 0UL || parsed > 65'535UL) {
                return false;
            }
            candidate.port = static_cast<std::uint16_t>(parsed);
            // An address that will not parse cannot be probed, and accepting it
            // would put a name lookup on the media path.
            sockaddr_in probe{};
            if (inet_pton(AF_INET, candidate.address.c_str(), &probe.sin_addr) != 1) {
                return false;
            }
            candidates.push_back(candidate);
            if (end == std::string::npos) {
                break;
            }
            at = end + 1U;
        }
        return !candidates.empty();
    }

    // Accepts both invite forms. JL1 names one address and is what earlier
    // builds emit; JL2 names every address this machine could be reached on.
    [[nodiscard]] static bool parseInviteCandidates(
        const std::string& invite,
        std::vector<IceCandidate>& candidates,
        std::array<std::uint8_t, 32U>& secret) {
        if (invite.rfind("JL2|", 0U) == 0U) {
            const auto listEnd = invite.find('|', 4U);
            if (listEnd == std::string::npos) {
                return false;
            }
            if (!hexDecode(invite.substr(listEnd + 1U), secret)) {
                return false;
            }
            return decodeCandidates(invite.substr(4U, listEnd - 4U), candidates);
        }
        std::string address;
        std::uint16_t port = 0U;
        if (!parseInvite(invite, address, port, secret)) {
            return false;
        }
        candidates.push_back(IceCandidate{address, port, CandidateKind::Host});
        return true;
    }

    [[nodiscard]] static sockaddr_in candidateAddress(const IceCandidate& candidate) noexcept {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(candidate.port);
        static_cast<void>(inet_pton(AF_INET, candidate.address.c_str(), &address.sin_addr));
        return address;
    }

    [[nodiscard]] static IceCandidate candidateFromAddress(const sockaddr_in& address) {
        char text[INET_ADDRSTRLEN]{};
        static_cast<void>(inet_ntop(AF_INET, &address.sin_addr, text, sizeof(text)));
        return IceCandidate{std::string(text), ntohs(address.sin_port), CandidateKind::Host};
    }

    [[nodiscard]] static bool parseInvite(
        const std::string& invite,
        std::string& address,
        std::uint16_t& port,
        std::array<std::uint8_t, 32U>& secret) noexcept {
        if (invite.rfind("JL1|", 0U) != 0U) {
            return false;
        }
        const auto addressEnd = invite.find('|', 4U);
        const auto portEnd = addressEnd == std::string::npos
            ? std::string::npos : invite.find('|', addressEnd + 1U);
        if (addressEnd == std::string::npos || portEnd == std::string::npos) {
            return false;
        }
        address = invite.substr(4U, addressEnd - 4U);
        const std::string portText = invite.substr(addressEnd + 1U, portEnd - addressEnd - 1U);
        unsigned long parsedPort = 0UL;
        try {
            std::size_t consumed = 0U;
            parsedPort = std::stoul(portText, &consumed);
            if (consumed != portText.size()) {
                return false;
            }
        } catch (...) {
            return false;
        }
        if (parsedPort == 0UL || parsedPort > 65'535UL
            || !hexDecode(invite.substr(portEnd + 1U), secret)) {
            return false;
        }
        port = static_cast<std::uint16_t>(parsedPort);
        return !address.empty();
    }

    [[nodiscard]] bool createBoundSocket(std::uint16_t requestedPort) {
        socket_.reset(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (!socket_) {
            return false;
        }
        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        local.sin_port = htons(requestedPort);
        if (bind(socket_.get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0) {
            socket_.reset();
            return false;
        }
        int localBytes = sizeof(local);
        if (getsockname(
                socket_.get(), reinterpret_cast<sockaddr*>(&local), &localBytes) != 0) {
            socket_.reset();
            return false;
        }
        localPort_ = ntohs(local.sin_port);
        u_long nonBlocking = 1UL;
        static_cast<void>(ioctlsocket(socket_.get(), FIONBIO, &nonBlocking));
        // Without this, an ICMP port-unreachable from any probed address makes
        // the next recvfrom fail with WSAECONNRESET on Windows.
        BOOL reportConnectionReset = FALSE;
        DWORD returned = 0U;
        static_cast<void>(WSAIoctl(
            socket_.get(), SIO_UDP_CONNRESET, &reportConnectionReset,
            sizeof(reportConnectionReset), nullptr, 0U, &returned, nullptr, nullptr));
        return true;
    }

    void launchWorker() {
        stopRequested_.store(false, std::memory_order_release);
        packetsSent_.store(0U, std::memory_order_relaxed);
        for (auto& limiter : sendLimiters_) {
            limiter.prepare(networkSampleRate);
        }
        packetsReceived_.store(0U, std::memory_order_relaxed);
        packetsRejected_.store(0U, std::memory_order_relaxed);
        iceRounds_.store(0U, std::memory_order_relaxed);
        iceProbes_.store(0U, std::memory_order_relaxed);
        // Per-slot counters are cleared by prepareSlotSession as each slot is
        // handed to somebody, which is the only moment a room-wide reset could
        // have been right about anyway.
        for (auto& held : peers_) {
            if (held != nullptr) {
                held->sessionsEstablished.store(0U, std::memory_order_relaxed);
            }
        }
        worker_ = std::thread([this] { run(); });
    }

    // The nonce is the slot's prefix and the slot's counter, which is why
    // this takes them rather than reading whichever peer happens to be first.
    // Two peers sharing a counter would interleave into one sequence and two
    // peers sharing a prefix would reuse a nonce under the room key -- and
    // both are invisible until there is a second musician to collide with.
    [[nodiscard]] std::size_t buildEncryptedPacket(
        AesGcmCipher& cipher,
        Direction direction,
        std::span<const std::uint8_t, noncePrefixBytes> noncePrefix,
        std::uint32_t& nonceCounter,
        bool& nonceExhausted,
        PacketType type,
        std::uint32_t sequence,
        std::uint32_t sampleRate,
        std::uint16_t frameCount,
        std::uint8_t stream,
        std::span<const std::uint8_t> plaintext,
        std::span<std::uint8_t, maximumDatagramBytes> destination) noexcept {
        if (plaintext.size() > maximumPlaintextBytes) {
            return 0U;
        }
        writeU32(destination.data(), protocolMagic);
        destination[4U] = protocolVersion;
        destination[5U] = static_cast<std::uint8_t>(type);
        writeU16(destination.data() + 6U, static_cast<std::uint16_t>(plaintext.size()));
        writeU32(destination.data() + 8U, sequence);
        writeU32(destination.data() + 12U, sampleRate);
        writeU16(destination.data() + 16U, frameCount);
        destination[18U] = static_cast<std::uint8_t>(direction);
        destination[19U] = stream;
        std::memcpy(destination.data() + 20U, noncePrefix.data(), noncePrefixBytes);
        if (nonceExhausted) {
            return 0U;
        }
        const std::uint32_t counter = ++nonceCounter;
        if (counter >= maximumNonceCounter) {
            // Never reuse a nonce. Refusing to send is the only safe response,
            // and it has to latch: without the flag the counter keeps climbing
            // on every refused attempt, wraps past 2^32, and starts handing out
            // nonces that have already been used under this key.
            nonceExhausted = true;
            return 0U;
        }
        writeU32(destination.data() + 20U + noncePrefixBytes, counter);
        auto ciphertext = destination.subspan(headerBytes, plaintext.size());
        auto tag = std::span<std::uint8_t, tagBytes>(
            destination.data() + headerBytes + plaintext.size(), tagBytes);
        if (!cipher.encrypt(destination.first(headerBytes), plaintext, ciphertext, tag)) {
            return 0U;
        }
        return headerBytes + plaintext.size() + tagBytes;
    }

    [[nodiscard]] bool sendPacket(
        PeerSlot& slot,
        RoomCiphers& roomCiphers,
        PacketType type,
        std::uint32_t sampleRate,
        std::uint16_t frameCount,
        std::span<const std::uint8_t> plaintext,
        AudioStreamId stream = AudioStreamId::Instrument,
        // Where to send, when it is not the peer we have settled on. Probing
        // several candidate addresses is the only way to find out which one a
        // pair of routers will actually carry.
        const sockaddr_in* destination = nullptr) noexcept {
        // A join request is sealed with the room key, because at that moment
        // this end has not heard from the other and cannot know its prefix.
        // Everything after it is sealed with that pair's key. That is the
        // whole rekey: one packet type, no negotiation, no extra round trip.
        AesGcmCipher& cipher = (type == PacketType::Hello || !slot.pairSend.has_value())
            ? roomCiphers.forDirection(slot.sendDirection) : *slot.pairSend;
        std::array<std::uint8_t, maximumDatagramBytes> packet{};
        // Media sequences are per stream so each receive buffer sees a
        // contiguous run. Replay protection uses the nonce counter, which is
        // unique across every packet in this direction.
        const std::uint32_t sequence = type == PacketType::Audio
            ? ++slot.sendSequence[streamIndex(stream)]
            : 0U;
        const std::size_t streamPosition = streamIndex(stream);
        const std::uint8_t streamField = type == PacketType::Audio
            ? static_cast<std::uint8_t>(
                streamPosition
                | (localSourceClipped_[streamPosition].load(std::memory_order_acquire) != 0U
                       ? sourceClipFlag : 0U))
            : std::uint8_t{0};
        const std::size_t bytes = buildEncryptedPacket(
            cipher, slot.sendDirection,
            std::span<const std::uint8_t, noncePrefixBytes>(slot.noncePrefix),
            slot.nonceCounter, slot.nonceExhausted,
            type, sequence, sampleRate, frameCount,
            streamField,
            plaintext, packet);
        if (bytes == 0U) {
            return false;
        }
        const int sent = sendto(
            socket_.get(), reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(bytes), 0,
            reinterpret_cast<const sockaddr*>(
                destination == nullptr ? &slot.remoteAddress : destination),
            sizeof(slot.remoteAddress));
        if (sent == static_cast<int>(bytes)) {
            packetsSent_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Tells somebody the room could not take them.
    //
    // There is no slot to send this from -- that is the whole message -- so it
    // carries the transport's own nonce prefix and counter, drawn once per
    // worker and shared with nothing. Sealed with the room key, like the join
    // request it answers, so the refused end can open it without a pair key it
    // was never going to be given.
    [[nodiscard]] bool sendRoomFull(
        RoomCiphers& roomCiphers,
        const sockaddr_in& destination,
        std::span<const std::uint8_t> encodedLocalParticipant) noexcept {
        std::array<std::uint8_t, maximumDatagramBytes> packet{};
        const std::size_t bytes = buildEncryptedPacket(
            roomCiphers.forDirection(Direction::HostToGuest),
            Direction::HostToGuest,
            std::span<const std::uint8_t, noncePrefixBytes>(refusalNoncePrefix_),
            refusalNonceCounter_, refusalNonceExhausted_,
            PacketType::RoomFull, 0U, 0U, 0U, 0U,
            encodedLocalParticipant, packet);
        if (bytes == 0U) {
            return false;
        }
        const int sent = sendto(
            socket_.get(), reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(bytes), 0,
            reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
        if (sent == static_cast<int>(bytes)) {
            packetsSent_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    // Sends the handshake to whichever candidate the agent says is due.
    //
    // The Hello is the probe. It is already authenticated with the room key and
    // already answered with a HelloAck, so a reply is proof of an authenticated
    // two-way path rather than merely of a packet arriving -- which is stronger
    // evidence than a bare ping, and needs no second packet type on the wire.
    void probeCandidates(
        PeerSlot& slot,
        RoomCiphers& roomCiphers,
        std::span<const std::uint8_t> encodedParticipant,
        ULONGLONG now) noexcept {
        const std::uint64_t nowMicroseconds = static_cast<std::uint64_t>(now) * 1'000ULL;
        // A router that refuses at one moment can cooperate at the next, and a
        // musician is still sitting there waiting. Exhausting the pairs starts
        // a fresh round rather than giving up, and each round is recorded so a
        // support bundle can show how many it took.
        if (slot.ice.exhausted(nowMicroseconds)) {
            iceRounds_.fetch_add(1U, std::memory_order_relaxed);
            JAMLINK_LOG("ice", "no candidate answered in this round; starting another");
            beginCandidateChecks(slot, nowMicroseconds);
        }
        // Bounded so a long stall cannot turn into a burst that looks like a
        // flood to a router already inclined to drop us.
        for (std::size_t attempt = 0U; attempt < 4U; ++attempt) {
            const IceAction action = slot.ice.nextAction(nowMicroseconds);
            if (!action.sendProbe) {
                return;
            }
            const sockaddr_in destination = candidateAddress(action.to);
            static_cast<void>(sendPacket(
                slot, roomCiphers, PacketType::Hello, 0U, 0U, encodedParticipant,
                AudioStreamId::Instrument, &destination));
            iceProbes_.fetch_add(1U, std::memory_order_relaxed);
        }
    }

    void beginCandidateChecks(PeerSlot& slot, std::uint64_t nowMicroseconds) {
        slot.ice.reset();
        // The address does not decide where a probe is sent from -- the bound
        // socket does -- but it does decide how pairs are ranked, and a pair
        // that never leaves the building should always be tried first.
        slot.ice.addLocalCandidate(
            IceCandidate{localIpv4Address(), localPort_, CandidateKind::Host});
        for (const auto& candidate : slot.remoteCandidates) {
            slot.ice.addRemoteCandidate(candidate);
        }
        slot.ice.beginChecks(nowMicroseconds);
    }

    void servicePendingChat(
        RoomCiphers& roomCiphers,
        ULONGLONG now) noexcept {
        const std::uint32_t connectedSlots = connectedSlotMask();
        if (connectedSlots == 0U) {
            return;
        }
        const std::scoped_lock lock(controlMutex_);
        std::size_t sentThisPass = 0U;
        for (auto iterator = pendingChat_.begin();
             iterator != pendingChat_.end() && sentThisPass < 2U;) {
            PendingChat& pending = *iterator;
            if (!pending.addressed) {
                pending.addressed = true;
                pending.awaitingSlots = connectedSlots;
            }
            // A musician who leaves before acknowledging cannot be waited
            // for. Reporting the message undelivered because one of three
            // walked away would describe the wrong thing to the two who did
            // receive it.
            pending.awaitingSlots &= connectedSlots;
            if (pending.awaitingSlots == 0U) {
                iterator = pendingChat_.erase(iterator);
                continue;
            }
            if (pending.attempts > 0U
                && now - pending.lastAttemptMilliseconds < chatRetryMilliseconds) {
                ++iterator;
                continue;
            }
            if (pending.attempts >= maximumChatAttempts) {
                if (controlEvents_.size() >= maximumControlEvents) {
                    controlEvents_.pop_front();
                }
                controlEvents_.push_back(RoomControlEvent{
                    RoomControlEventType::ChatDeliveryFailed,
                    pending.messageId,
                    pending.timestampMilliseconds,
                    localParticipant_,
                    pending.text});
                iterator = pendingChat_.erase(iterator);
                continue;
            }
            std::array<std::uint8_t, 18U + maximumChatMessageBytes> payload{};
            writeU64(payload.data(), pending.messageId);
            writeU64(payload.data() + 8U, pending.timestampMilliseconds);
            writeU16(payload.data() + 16U, static_cast<std::uint16_t>(pending.text.size()));
            std::memcpy(payload.data() + 18U, pending.text.data(), pending.text.size());
            const auto body = std::span<const std::uint8_t>(
                payload.data(), 18U + pending.text.size());
            // Only to the musicians who have not acknowledged it. Sending to
            // everybody on every retry would turn one slow link into a
            // retransmission the whole room pays for.
            for (std::size_t index = 0U; index < maximumPeerSlots; ++index) {
                if ((pending.awaitingSlots & (1U << index)) == 0U) {
                    continue;
                }
                static_cast<void>(sendPacket(
                    peer(index), roomCiphers, PacketType::Chat, 0U, 0U, body));
            }
            pending.lastAttemptMilliseconds = now;
            ++pending.attempts;
            ++sentThisPass;
            ++iterator;
        }
    }

    void acknowledgeChat(
        PeerSlot& slot, RoomCiphers& roomCiphers, std::uint64_t messageId) noexcept {
        std::array<std::uint8_t, 8U> acknowledgement{};
        writeU64(acknowledgement.data(), messageId);
        static_cast<void>(sendPacket(
            slot, roomCiphers, PacketType::ChatAck, 0U, 0U, acknowledgement));
    }

    void receiveChatAcknowledgement(const PeerSlot& slot, std::uint64_t messageId) {
        const std::uint32_t bit = 1U << slotIndexOf(slot);
        const std::scoped_lock lock(controlMutex_);
        const auto found = std::find_if(
            pendingChat_.begin(), pendingChat_.end(),
            [messageId](const PendingChat& pending) {
                return pending.messageId == messageId;
            });
        if (found == pendingChat_.end()) {
            return;
        }
        // One musician has it. The message stays until everybody addressed
        // does; the next service pass erases it once nobody is left owing.
        found->awaitingSlots &= ~bit;
        if (found->addressed && found->awaitingSlots == 0U) {
            pendingChat_.erase(found);
        }
    }

    [[nodiscard]] bool receiveChatMessage(
        PeerSlot& slot,
        RoomCiphers& roomCiphers,
        std::span<const std::uint8_t> payload) {
        if (payload.size() < 18U) {
            return false;
        }
        const std::uint64_t messageId = readU64(payload.data());
        const std::uint64_t timestamp = readU64(payload.data() + 8U);
        const std::size_t textBytes = readU16(payload.data() + 16U);
        if (messageId == 0U || textBytes == 0U || textBytes > maximumChatMessageBytes
            || payload.size() != 18U + textBytes) {
            return false;
        }
        const std::string text(
            reinterpret_cast<const char*>(payload.data() + 18U), textBytes);
        if (!validUtf8(text, true)) {
            return false;
        }
        acknowledgeChat(slot, roomCiphers, messageId);
        // Keyed by sender as well as by id. Every musician numbers their own
        // messages from one, so an id on its own collides across a room, and
        // the collision drops the second person's message as a duplicate of
        // the first without anything saying so.
        if (!rememberChatMessage(chatKey(slot.remoteParticipant.profileId, messageId))) {
            return true;
        }

        const ULONGLONG now = GetTickCount64();
        while (!inboundChatTimes_.empty()
               && now - inboundChatTimes_.front() >= chatRateWindowMilliseconds) {
            inboundChatTimes_.pop_front();
        }
        if (inboundChatTimes_.size() >= maximumChatMessagesPerWindow) {
            return true;
        }
        inboundChatTimes_.push_back(now);
        appendControlEvent(RoomControlEvent{
            RoomControlEventType::ChatMessage,
            messageId,
            timestamp,
            slotParticipant(slot),
            text});
        return true;
    }

    // Everything that happens once per pass, for every musician in the room.
    //
    // This was written around exactly one peer: one handshake, one connected
    // flag, one timeout, one probe schedule, one cipher pair. Each of those is
    // now per slot, and the loop services every slot that somebody holds. The
    // one thing that deliberately did not become per peer is the capture and
    // encode side, which is shared -- see drainOutgoingAudio.
    void run() noexcept {
        try {
            // Both directions, because a musician can be the host of one pair
            // and the guest of another in the same room. Which one a slot uses
            // is the slot's own business.
            std::array<std::uint8_t, 32U> hostToGuestKey{};
            std::array<std::uint8_t, 32U> guestToHostKey{};
            if (!deriveDirectionKey(secret_, Direction::HostToGuest, hostToGuestKey)
                || !deriveDirectionKey(secret_, Direction::GuestToHost, guestToHostKey)) {
                state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
                return;
            }
            RoomCiphers roomCiphers{
                AesGcmCipher(hostToGuestKey), AesGcmCipher(guestToHostKey)};
            SecureZeroMemory(hostToGuestKey.data(), hostToGuestKey.size());
            SecureZeroMemory(guestToHostKey.data(), guestToHostKey.size());
            refusalNonceCounter_ = 0U;
            refusalNonceExhausted_ = false;
            if (!roomCiphers.valid()
                || !BCRYPT_SUCCESS(BCryptGenRandom(
                    nullptr, refusalNoncePrefix_.data(),
                    static_cast<ULONG>(refusalNoncePrefix_.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
                return;
            }
            const PeerParticipantInfo localParticipant = localParticipantSnapshot();
            std::array<std::uint8_t, 512U> participantPayload{};
            const std::size_t participantBytes = encodeParticipant(
                localParticipant, participantPayload);
            if (participantBytes == 0U) {
                state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
                return;
            }
            const auto encodedParticipant = std::span<const std::uint8_t>(
                participantPayload.data(), participantBytes);
            // Shared, and deliberately so. A mesh encodes a packet once and
            // seals it separately for each recipient, so everything on the
            // near side of the encoder belongs to this machine rather than to
            // any one peer.
            std::array<JamLinkStreamEncoder, audioStreamCount> outgoingEncoders{
                JamLinkStreamEncoder(
                    networkSampleRate, networkPacketFrames, outgoingBitsPerSecond,
                    audio::OpusStreamEncoder::Content::Music),
                JamLinkStreamEncoder(
                    networkSampleRate, networkPacketFrames, outgoingBitsPerSecond,
                    audio::OpusStreamEncoder::Content::Voice)};
            for (auto& encoder : outgoingEncoders) {
                encoder.setCodec(
                    static_cast<PeerAudioCodec>(
                        preferredCodec_.load(std::memory_order_acquire)));
            }
            std::array<OutgoingAudioPacer, audioStreamCount> outgoingPacers{
                OutgoingAudioPacer(
                    networkPacketFrames, networkSampleRate, maximumOutgoingBacklogFrames),
                OutgoingAudioPacer(
                    networkPacketFrames, networkSampleRate, maximumOutgoingBacklogFrames)};
            std::array<std::uint32_t, audioStreamCount> outgoingRates{};
            std::array<std::uint32_t, audioStreamCount> encoderBitrates{};
            for (std::size_t index = 0U; index < audioStreamCount; ++index) {
                static_cast<void>(outgoingPacers[index].setSourceRate(networkSampleRate));
                outgoingRates[index] = networkSampleRate;
                encoderBitrates[index] = outgoingBitsPerSecond;
            }
            std::array<float, 1'024U> localScratch{};
            std::array<float, networkPacketFrames> networkFloat{};
            std::array<std::uint8_t, maximumAudioPayloadBytes> networkPcm{};
            std::array<std::uint8_t, maximumDatagramBytes> receivedPacket{};
            std::array<std::uint8_t, maximumPlaintextBytes> decrypted{};
            ULONGLONG lastSummary = GetTickCount64();
            std::size_t loggedConnected = 0U;
            JAMLINK_LOG("session", hostMode_ ? "worker started as host"
                                             : "worker started as guest");

            while (!stopRequested_.load(std::memory_order_acquire)) {
                const ULONGLONG now = GetTickCount64();
                const bool versionMismatch = state_.load(std::memory_order_acquire)
                    == PeerConnectionState::VersionMismatch;

                // Anybody the roster names that this end has not reached yet.
                // Nothing distributes a roster today, so in a duo this finds
                // nobody -- it is what turns an introduction into a session
                // once introductions exist.
                openSessionsTheRosterNames(localParticipant, now);

                forEachSlot([&](PeerSlot& slot) {
                    if (versionMismatch) {
                        return;
                    }
                    // Reaching out, or waiting to be reached. Which of the two
                    // is per pair now: this machine can be the guest of one
                    // musician and the host of another at the same time.
                    if (!slot.hostRole && !slot.connected) {
                        probeCandidates(slot, roomCiphers, encodedParticipant, now);
                    }
                    // The host punches toward the guest as soon as it knows
                    // where the guest is. Without this the host's router never
                    // opens a mapping for the guest and drops every Hello,
                    // which is exactly how a direct connection fails with both
                    // public addresses discovered and both invites correct.
                    if (slot.hostRole && !slot.connected
                        && slot.remoteEndpointKnown.load(std::memory_order_acquire)
                        && now - slot.lastHello >= 250U) {
                        static_cast<void>(sendPacket(
                            slot, roomCiphers, PacketType::Punch, 0U, 0U, {}));
                        slot.lastHello = now;
                    }
                    // Sent once per session rather than repeatedly: addresses
                    // do not change while a session holds, and a session that
                    // drops and re-forms sends it again because this resets
                    // with the rest of the connection state.
                    if (slot.connected && !slot.publishedCandidates) {
                        std::array<std::uint8_t, 512U> rosterPayload{};
                        const std::size_t rosterBytes = encodeCandidateReport(
                            localParticipant, rosterPayload);
                        if (rosterBytes != 0U
                            && sendPacket(
                                slot, roomCiphers, PacketType::Candidates, 0U, 0U,
                                std::span<const std::uint8_t>(
                                    rosterPayload.data(), rosterBytes))) {
                            slot.publishedCandidates = true;
                        }
                    }
                    if (slot.connected && now - slot.lastPing >= 500U) {
                        sendControlPing(slot, roomCiphers);
                        slot.lastPing = now;
                    }
                });

                const std::size_t connectedNow = connectedPeerCount();
                if (connectedNow != loggedConnected) {
                    loggedConnected = connectedNow;
                    JAMLINK_LOG("session", "musicians in session: "
                        + std::to_string(connectedNow));
                }
                if (now - lastSummary >= 5'000U) {
                    lastSummary = now;
                    logSessionSummary(now);
                }

                drainOutgoingAudio(
                    roomCiphers, outgoingPacers, outgoingEncoders, outgoingRates,
                    encoderBitrates, localScratch, networkFloat, networkPcm);
                servicePendingChat(roomCiphers, now);

                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(socket_.get(), &readSet);
                timeval timeout{0, 5'000};
                const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
                if (selected > 0 && FD_ISSET(socket_.get(), &readSet)) {
                    constexpr std::size_t maximumDatagramsPerCycle = 64U;
                    for (std::size_t datagram = 0U;
                         datagram < maximumDatagramsPerCycle; ++datagram) {
                        sockaddr_in source{};
                        int sourceBytes = sizeof(source);
                        const int bytes = recvfrom(
                            socket_.get(), reinterpret_cast<char*>(receivedPacket.data()),
                            static_cast<int>(receivedPacket.size()), 0,
                            reinterpret_cast<sockaddr*>(&source), &sourceBytes);
                        if (bytes < 0) {
                            const int error = WSAGetLastError();
                            if (error == WSAEWOULDBLOCK) {
                                break;
                            }
                            // These are per-datagram conditions, not socket
                            // failures. WSAEMSGSIZE in particular means a
                            // datagram larger than any JamLink packet arrived,
                            // which anyone who knows the port can send;
                            // treating it as fatal made the session remotely
                            // killable without the room secret.
                            if (error == WSAEMSGSIZE || error == WSAECONNRESET
                                || error == WSAENETRESET || error == WSAEINTR) {
                                packetsRejected_.fetch_add(1U, std::memory_order_relaxed);
                                continue;
                            }
                            state_.store(
                                PeerConnectionState::SocketFailed, std::memory_order_release);
                            return;
                        }
                        if (handlePacket(
                                roomCiphers,
                                std::span<const std::uint8_t>(receivedPacket.data(),
                                    static_cast<std::size_t>(bytes)),
                                decrypted, source, localParticipant,
                                encodedParticipant)) {
                            packetsReceived_.fetch_add(1U, std::memory_order_relaxed);
                        } else {
                            packetsRejected_.fetch_add(1U, std::memory_order_relaxed);
                        }
                    }
                }
                expireSilentPeers();
            }
        } catch (...) {
            state_.store(PeerConnectionState::SocketFailed, std::memory_order_release);
        }
    }

    // The periodic control packet, per musician.
    //
    // The concealment figures in it are what the far end uses to decide its
    // own send rate, and they are a property of one link: reporting the room's
    // average would have every musician lower their rate because one person's
    // connection is poor.
    void sendControlPing(PeerSlot& slot, RoomCiphers& roomCiphers) noexcept {
        // Eight bytes of timestamp for the round trip, then one byte of
        // per-stream mute state. Mute has to travel here rather than on the
        // audio packets that carry the clip flag, because a muted stream sends
        // no audio packets at all. Then one byte per stream saying what
        // fraction of it this end has had to conceal since the last report:
        // the far end is the only witness to what our uplink is doing, so its
        // send rate has to be decided from what we tell it here rather than
        // from anything it can measure on its own.
        std::array<std::uint8_t, 9U + audioStreamCount> controlPayload{};
        const std::uint64_t stamp = nowMicroseconds();
        std::memcpy(controlPayload.data(), &stamp, sizeof(stamp));
        std::uint8_t muteMask = 0U;
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            if (sendMuted_.load(std::memory_order_acquire) != 0U
                || localStreamMuted_[index].load(std::memory_order_acquire) != 0U) {
                muteMask |= static_cast<std::uint8_t>(1U << index);
            }
        }
        controlPayload[sizeof(stamp)] = muteMask;
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            const auto stream = slot.receivers[index].telemetry();
            const std::uint64_t accepted =
                stream.packetsAccepted - slot.lastAccepted[index];
            const std::uint64_t concealed =
                stream.packetsConcealed - slot.lastConcealed[index];
            slot.lastAccepted[index] = stream.packetsAccepted;
            slot.lastConcealed[index] = stream.packetsConcealed;
            const std::uint64_t total = accepted + concealed;
            // A window with almost nothing in it says nothing about the link.
            // A percentage of three packets would drop the rate on a stream
            // that is merely muted.
            controlPayload[sizeof(stamp) + 1U + index] = total < 20U
                ? std::uint8_t{0}
                : static_cast<std::uint8_t>(
                    std::min<std::uint64_t>(concealed * 100U / total, 100U));
        }
        static_cast<void>(sendPacket(
            slot, roomCiphers, PacketType::Ping, 0U, 0U, controlPayload));
    }

    // Lets one musician time out without disturbing the others.
    //
    // A single receive deadline would have timed the whole room out on the
    // silence of whoever left. The slot and its addresses are kept: they are
    // what lets a dropped session re-form without a fresh invite, and they
    // cost nothing until somebody else needs the seat.
    void expireSilentPeers() {
        const ULONGLONG now = GetTickCount64();
        forEachSlot([&](PeerSlot& slot) {
            if (!slot.connected || now - slot.lastReceive <= 5'000U) {
                return;
            }
            slot.connected = false;
            slot.publishedCandidates = false;
            appendControlEvent(RoomControlEvent{
                RoomControlEventType::PeerLeft, 0U, systemTimeMilliseconds(),
                slotParticipant(slot), "Connection lost"});
        });
        refreshRoomState();
    }

    // What the room reports about itself.
    //
    // Connected means at least one other musician is in session, which is the
    // answer to "can I play at all" -- a room where one of three has dropped
    // is still a room the other two can play in, and calling it disconnected
    // would stop a session that is working. Whether everybody the roster names
    // is here is a different question, answered by telemetry().roomComplete.
    void refreshRoomState() noexcept {
        const PeerConnectionState current = state_.load(std::memory_order_acquire);
        // A broken socket or a failed key is not something anybody arriving
        // can put right, so those stand whatever the room does.
        if (current == PeerConnectionState::SocketFailed
            || current == PeerConnectionState::EncryptionFailed
            || current == PeerConnectionState::InviteInvalid) {
            return;
        }
        if (connectedPeerCount() > 0U) {
            state_.store(PeerConnectionState::Connected, std::memory_order_release);
            return;
        }
        // Verdicts the far end actually gave us. They are kept rather than
        // rewritten back to "connecting" on the next pass, because a musician
        // who has been told the build is wrong or the room is full should not
        // watch that turn back into a spinner -- but they are kept only while
        // nothing is connected, so a later attempt that succeeds says so.
        if (current == PeerConnectionState::VersionMismatch
            || current == PeerConnectionState::RoomFull) {
            return;
        }
        state_.store(
            hostMode_ ? PeerConnectionState::WaitingForPeer
                      : PeerConnectionState::Connecting,
            std::memory_order_release);
    }

    // Opens a session with anybody the roster names and this end has not
    // reached yet.
    //
    // The roster answers the question that has no obvious owner: which end of
    // a pair reaches out. Both ends run the same comparison over the same two
    // participant identifiers and reach opposite answers with nothing
    // exchanged, so a pair cannot end up with two hosts or two guests -- both
    // of which are a session that silently never forms.
    //
    // Idempotent, because the roster arrives on every change and again on
    // every reconnect, and a room must not accumulate duplicate sessions with
    // the same person.
    void openSessionsTheRosterNames(
        const PeerParticipantInfo& localParticipant, ULONGLONG now) {
        std::vector<PendingPeer> pending;
        {
            const std::scoped_lock lock(rosterMutex_);
            if (roster_.size() == 0U) {
                return;
            }
            std::vector<std::string> alreadyConnected;
            forEachSlot([&alreadyConnected](const PeerSlot& slot) {
                if (!slot.remoteParticipant.profileId.empty()) {
                    alreadyConnected.push_back(slot.remoteParticipant.profileId);
                }
            });
            pending = roster_.peersToConnect(
                localParticipant.profileId, alreadyConnected);
        }
        for (const auto& peerToOpen : pending) {
            // Only the end the roster says reaches out actually does. The
            // other waits, exactly as a host does today.
            if (peerToOpen.role != PairRole::Guest || peerToOpen.member.candidates.empty()) {
                continue;
            }
            if (!canAdmitAnotherMusician()) {
                return;
            }
            PeerSlot* slot = allocateSlot();
            if (slot == nullptr || !prepareSlotSession(*slot, false)) {
                return;
            }
            slot->remoteCandidates = peerToOpen.member.candidates;
            slot->remoteAddress = candidateAddress(peerToOpen.member.candidates.front());
            slot->remoteEndpointKnown.store(true, std::memory_order_release);
            {
                const std::scoped_lock lock(controlMutex_);
                // Named before the handshake proves it, so the same person is
                // not dialled twice on the next pass. Nothing is believed
                // about them until a packet authenticates.
                slot->remoteParticipant.profileId = peerToOpen.member.participantId;
            }
            beginCandidateChecks(*slot, static_cast<std::uint64_t>(now) * 1'000ULL);
            JAMLINK_LOG("mesh", "opening a session with a musician the room introduced");
        }
    }

    void logSessionSummary(ULONGLONG now) {
        const std::uint64_t received = packetsReceived_.load(std::memory_order_relaxed);
        const std::uint64_t rejected = packetsRejected_.load(std::memory_order_relaxed);
        const std::uint64_t sent = packetsSent_.load(std::memory_order_relaxed);
        if (connectedPeerCount() == 0U) {
            const PeerSlot& waiting = peer();
            JAMLINK_LOG("stalled", "nobody connected after "
                + std::to_string((now - waiting.lastReceive) / 1000U) + "s: sent "
                + std::to_string(sent) + ", received " + std::to_string(received)
                + ", rejected " + std::to_string(rejected)
                + (received == 0U
                    ? std::string(" - nothing has arrived at all, which points at"
                        " the router discarding it rather than at JamLink")
                    : std::string(" - packets are arriving but the handshake has"
                        " not completed")));
            return;
        }
        // Per stream and per musician, because a total that looks like exactly
        // one stream's worth is the difference between a lossy link and a
        // stream that never reaches the wire at all, and a room-wide figure
        // cannot tell those apart or say whose link it was.
        JAMLINK_LOG("session", "sent " + std::to_string(sent)
            + ", received " + std::to_string(received)
            + ", rejected " + std::to_string(rejected));
        forEachSlot([this](const PeerSlot& slot) {
            if (!slot.connected) {
                return;
            }
            const auto instrument =
                slot.receivers[streamIndex(AudioStreamId::Instrument)].telemetry();
            const auto voice =
                slot.receivers[streamIndex(AudioStreamId::Voice)].telemetry();
            JAMLINK_LOG("session", "peer " + std::to_string(slotIndexOf(slot))
                + ": instrument in " + std::to_string(instrument.packetsAccepted)
                + " concealed " + std::to_string(instrument.packetsConcealed)
                + " buffer " + std::to_string(instrument.currentDepthFrames / 48U)
                + "ms; voice in " + std::to_string(voice.packetsAccepted)
                + " concealed " + std::to_string(voice.packetsConcealed)
                + " buffer " + std::to_string(voice.currentDepthFrames / 48U)
                + "ms; out instrument " + std::to_string(slot.sendSequence[0])
                + " voice " + std::to_string(slot.sendSequence[1]));
        });
    }

    // Capture, encode, and send to everybody.
    //
    // THE HAZARD, and it only appears with three people. The pacer holds the
    // release schedule and hands over one packet's worth of audio at a time.
    // Draining it once per peer would give the first musician everything and
    // every musician after them silence -- and no two-person test could ever
    // show it, because with one peer the two shapes are identical.
    //
    // So the packet is captured once, limited once and encoded once, out here
    // where the pacer is; only the sealing happens per recipient, which is
    // what a mesh actually requires, since a packet has to be bound to the
    // peer it is addressed to.
    void drainOutgoingAudio(
        RoomCiphers& roomCiphers,
        std::array<OutgoingAudioPacer, audioStreamCount>& pacers,
        std::array<JamLinkStreamEncoder, audioStreamCount>& encoders,
        std::array<std::uint32_t, audioStreamCount>& configuredRates,
        std::array<std::uint32_t, audioStreamCount>& encoderBitrates,
        std::array<float, 1'024U>& localScratch,
        std::array<float, networkPacketFrames>& networkFloat,
        std::array<std::uint8_t, maximumAudioPayloadBytes>& networkPcm) noexcept {
        const std::uint32_t recipients = connectedSlotMask();
        if (recipients == 0U) {
            return;
        }
        const std::uint64_t nowMicros = nowMicroseconds();
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            auto& pacer = pacers[index];
            const std::uint32_t requestedRate =
                localSampleRate_[index].load(std::memory_order_relaxed);
            // A rate the converter cannot honour leaves the stream on the one it
            // already had rather than stopping it.
            if (requestedRate != configuredRates[index]
                && pacer.setSourceRate(requestedRate)) {
                configuredRates[index] = requestedRate;
            }
            while (localAudio_[index].availableReadFrames() > 0U) {
                const std::size_t frames = std::min(
                    localScratch.size(), localAudio_[index].availableReadFrames());
                static_cast<void>(localAudio_[index].readAndZeroFill(
                    std::span<float>(localScratch.data(), frames)));
                pacer.accept(std::span<const float>(localScratch.data(), frames));
            }
            // One encoder, so one rate, so the lowest rate anybody asked for.
            // Encoding separately per peer to spare the healthy links a
            // smaller packet would multiply the most expensive thing the send
            // path does by the size of the room; a musician whose link cannot
            // carry 96 kbit/s would otherwise simply not be sent to.
            //
            // Applied here rather than where the report arrived, because the
            // encoder belongs to this loop and reconfiguring it from the
            // packet handler would be a second thread touching it.
            std::uint32_t lowestRequested = 0U;
            forEachSlot([&lowestRequested, index](PeerSlot& slot) {
                if (!slot.connected) {
                    return;
                }
                const std::uint32_t reported =
                    slot.pendingBitrate[index].exchange(0U, std::memory_order_acquire);
                if (reported != 0U) {
                    slot.requestedBitrate[index] = reported;
                }
                if (slot.requestedBitrate[index] != 0U
                    && (lowestRequested == 0U
                        || slot.requestedBitrate[index] < lowestRequested)) {
                    lowestRequested = slot.requestedBitrate[index];
                }
            });
            if (lowestRequested != 0U && lowestRequested != encoderBitrates[index]) {
                static_cast<void>(encoders[index].setBitsPerSecond(lowestRequested));
                encoderBitrates[index] = lowestRequested;
                JAMLINK_LOG("codec", "stream " + std::to_string(index)
                    + " send rate now " + std::to_string(lowestRequested) + " bit/s");
            }
            // The pacer holds the schedule: it releases on the cadence the audio
            // itself represents, makes up lateness by sending sooner rather than
            // by abandoning what was captured, and counts anything it does have
            // to drop. The loop ends of its own accord when nothing is due.
            while (pacer.release(
                nowMicros, std::span<float>(networkFloat.data(), networkFloat.size()))) {
                // Last thing before the encoder, and only on what leaves the
                // machine. The monitor, the recording, and above all the
                // pristine local originals never see this: the originals are
                // what was played and have to stay lossless.
                sendLimiters_[index].process(
                    std::span<float>(networkFloat.data(), networkFloat.size()));
                const std::size_t written = encoders[index].encode(
                    std::span<const float>(networkFloat.data(), networkFloat.size()),
                    std::span<std::uint8_t>(networkPcm));
                if (written == 0U) {
                    // Nothing usable came out. Counted where the pacer's own
                    // accounting can see it rather than dropped silently.
                    encodeFailures_.fetch_add(1U, std::memory_order_relaxed);
                    continue;
                }
                const auto encoded =
                    std::span<const std::uint8_t>(networkPcm.data(), written);
                // Sealed separately for each musician, from the recipient list
                // taken before the release loop began. A peer that arrives
                // part way through this pass starts at the next packet rather
                // than at half of this one.
                for (std::size_t target = 0U; target < maximumPeerSlots; ++target) {
                    if ((recipients & (1U << target)) == 0U) {
                        continue;
                    }
                    static_cast<void>(sendPacket(
                        peer(target), roomCiphers, PacketType::Audio, networkSampleRate,
                        static_cast<std::uint16_t>(networkPacketFrames),
                        encoded, static_cast<AudioStreamId>(index)));
                }
            }
        }
    }

    // Builds this pair's ciphers from the two nonce prefixes.
    //
    // Both ends sort the pair before hashing it, so each computes the same key
    // without having to agree which of them is first. Nothing is committed to
    // the slot until a packet has actually authenticated under the result --
    // otherwise anyone able to send a datagram could replace a live session's
    // keys just by putting a different prefix in a header.
    [[nodiscard]] bool buildPairCiphers(
        const PeerSlot& slot,
        std::span<const std::uint8_t, noncePrefixBytes> remotePrefix,
        std::optional<AesGcmCipher>& sendOut,
        std::optional<AesGcmCipher>& receiveOut) noexcept {
        std::array<std::uint8_t, 32U> sendKey{};
        std::array<std::uint8_t, 32U> receiveKey{};
        const auto asDirection = [](Direction direction) {
            return direction == Direction::HostToGuest
                ? KeyDirection::HostToGuest : KeyDirection::GuestToHost;
        };
        const bool derived = derivePeerKey(
                std::span<const std::uint8_t, 32U>(secret_),
                std::span<const std::uint8_t, noncePrefixBytes>(slot.noncePrefix),
                remotePrefix, asDirection(slot.sendDirection),
                std::span<std::uint8_t, 32U>(sendKey))
            && derivePeerKey(
                std::span<const std::uint8_t, 32U>(secret_),
                std::span<const std::uint8_t, noncePrefixBytes>(slot.noncePrefix),
                remotePrefix, asDirection(slot.receiveDirection),
                std::span<std::uint8_t, 32U>(receiveKey));
        if (derived) {
            sendOut.emplace(std::span<const std::uint8_t, 32U>(sendKey));
            receiveOut.emplace(std::span<const std::uint8_t, 32U>(receiveKey));
        }
        SecureZeroMemory(sendKey.data(), sendKey.size());
        SecureZeroMemory(receiveKey.data(), receiveKey.size());
        return derived && sendOut->valid() && receiveOut->valid();
    }

    // "profile-id\ndisplay name\ncandidates". The candidate text is the same
    // form an invite carries, so there is one encoding of an address in the
    // project rather than two that can drift apart.
    [[nodiscard]] std::size_t encodeCandidateReport(
        const PeerParticipantInfo& participant,
        std::span<std::uint8_t> destination) const {
        std::vector<IceCandidate> mine;
        const std::string localAddress = localIpv4Address();
        if (!localAddress.empty() && localPort_ != 0U) {
            mine.push_back(IceCandidate{localAddress, localPort_, CandidateKind::Host});
        }
        // Whatever a router told us about ourselves, when it did. A guest never
        // ran this for its own sake before, because nobody had to find it.
        if (!publicAddress_.empty() && publicPort_ != 0U) {
            mine.push_back(IceCandidate{
                publicAddress_, publicPort_, CandidateKind::ServerReflexive});
        }
        if (mine.empty()) {
            return 0U;
        }
        const std::string text = participant.profileId + "\n"
            + participant.displayName + "\n" + encodeCandidates(mine);
        if (text.size() > destination.size()) {
            return 0U;
        }
        std::memcpy(destination.data(), text.data(), text.size());
        return text.size();
    }

    [[nodiscard]] static bool decodeCandidateReport(
        std::span<const std::uint8_t> payload, RosterMember& member) {
        const std::string text(
            reinterpret_cast<const char*>(payload.data()), payload.size());
        const auto firstBreak = text.find('\n');
        const auto secondBreak = firstBreak == std::string::npos
            ? std::string::npos : text.find('\n', firstBreak + 1U);
        if (firstBreak == std::string::npos || secondBreak == std::string::npos) {
            return false;
        }
        member.participantId = text.substr(0U, firstBreak);
        member.displayName = text.substr(firstBreak + 1U, secondBreak - firstBreak - 1U);
        member.candidates.clear();
        return !member.participantId.empty()
            && decodeCandidates(text.substr(secondBreak + 1U), member.candidates);
    }

    // Which musician a datagram came from, and what to do about it.
    //
    // An arriving packet used to be assumed to come from THE peer. It has to
    // be attributed to one instead, and the order that happens in is the whole
    // security of it:
    //
    //   1. the source address against the slots somebody already holds;
    //   2. anything that matches nothing and is not a join request: rejected;
    //   3. a join request that matches nothing: asks for a slot.
    //
    // The other way round -- deciding from what the packet claims to be and
    // checking the address afterwards -- would let anyone take a slot by
    // sending anything at all.
    [[nodiscard]] bool handlePacket(
        RoomCiphers& roomCiphers,
        std::span<const std::uint8_t> packet,
        std::array<std::uint8_t, maximumPlaintextBytes>& plaintext,
        const sockaddr_in& source,
        const PeerParticipantInfo& localParticipant,
        std::span<const std::uint8_t> encodedLocalParticipant) noexcept {
        if (packet.size() < headerBytes + tagBytes
            || readU32(packet.data()) != protocolMagic
            || packet[4U] != protocolVersion
            || (packet[19U] & streamIndexMask) >= audioStreamCount) {
            return false;
        }
        const std::size_t payloadBytes = readU16(packet.data() + 6U);
        if (payloadBytes > plaintext.size()
            || packet.size() != headerBytes + payloadBytes + tagBytes) {
            return false;
        }
        const PacketType type = static_cast<PacketType>(packet[5U]);
        PeerSlot* slot = slotForPacket(source, type);
        if (slot != nullptr) {
            return handlePacketForSlot(
                *slot, roomCiphers, packet, plaintext, source, localParticipant,
                encodedLocalParticipant);
        }
        if (type != PacketType::Hello) {
            return false;
        }
        return admitStranger(
            roomCiphers, packet, plaintext, source, localParticipant,
            encodedLocalParticipant);
    }

    // A join request from somebody who holds no slot.
    //
    // Authenticated before a slot is allocated, not after. Allocating first
    // would let anyone who can reach the port take every seat in the room with
    // datagrams full of nothing; requiring the room secret first costs one
    // decryption per join and makes that impossible. It happens once when a
    // musician arrives, never per packet.
    [[nodiscard]] bool admitStranger(
        RoomCiphers& roomCiphers,
        std::span<const std::uint8_t> packet,
        std::array<std::uint8_t, maximumPlaintextBytes>& plaintext,
        const sockaddr_in& source,
        const PeerParticipantInfo& localParticipant,
        std::span<const std::uint8_t> encodedLocalParticipant) noexcept {
        // A stranger's join request can only be one thing: somebody asking
        // this machine to be their pair's host. Anything claiming the other
        // direction is this end's own traffic reflected back.
        if (packet[18U] != static_cast<std::uint8_t>(Direction::GuestToHost)) {
            return false;
        }
        // This machine's own join request, handed back to it. A hairpinning
        // router or a relay will do that without anybody meaning any harm, and
        // it authenticates perfectly, because it was sealed with the room key
        // this end holds -- so nothing about the payload can catch it. Taking
        // a slot for it would open a session against ourselves, and repeating
        // that would fill the room with them.
        //
        // Recognised by the nonce prefix rather than by the identity in it.
        // The prefix is drawn per slot from the system generator and is
        // genuinely this machine's; the identity is not, because two people
        // running an unconfigured build legitimately share one.
        if (isOwnNoncePrefix(std::span<const std::uint8_t, noncePrefixBytes>(
                packet.data() + 20U, noncePrefixBytes))) {
            return false;
        }
        const std::size_t payloadBytes = readU16(packet.data() + 6U);
        const auto ciphertext = packet.subspan(headerBytes, payloadBytes);
        const auto tag = std::span<const std::uint8_t, tagBytes>(
            packet.data() + headerBytes + payloadBytes, tagBytes);
        if (!roomCiphers.forDirection(Direction::GuestToHost).decrypt(
                packet.first(headerBytes), ciphertext, tag,
                std::span<std::uint8_t>(plaintext.data(), payloadBytes))) {
            return false;
        }
        PeerParticipantInfo proposed;
        if (!decodeParticipant(
                std::span<const std::uint8_t>(plaintext.data(), payloadBytes), proposed)) {
            return false;
        }
        // Somebody already here under that name. A bearer invite plus a
        // copied, self-asserted profile ID must not buy a second seat beside
        // the person it was copied from, so this is handed to the slot that
        // name already belongs to -- which will refuse it while that musician
        // is healthy, and accept it once they have timed out.
        if (PeerSlot* existing = slotForParticipant(proposed.profileId)) {
            return handlePacketForSlot(
                *existing, roomCiphers, packet, plaintext, source, localParticipant,
                encodedLocalParticipant);
        }
        // Asked of the capacity guard rather than answered again here, and
        // asked before a slot is taken rather than after.
        PeerSlot* slot = canAdmitAnotherMusician() ? allocateSlot() : nullptr;
        if (slot == nullptr) {
            // Told, not ignored. Silence is indistinguishable from a network
            // that never carried them, and somebody watching a spinner has no
            // way to tell "the room is full" from "JamLink is broken".
            static_cast<void>(sendRoomFull(roomCiphers, source, encodedLocalParticipant));
            JAMLINK_LOG("room", "refused a musician: the room cannot carry another");
            return true;
        }
        if (!prepareSlotSession(*slot, true)) {
            state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
            return false;
        }
        slot->remoteAddress = source;
        slot->remoteEndpointKnown.store(true, std::memory_order_release);
        // Handed to the same path every other packet takes, rather than a
        // second copy of the handshake that could drift from it. The
        // decryption above is repeated there, once, on the packet that admits
        // somebody to the room.
        return handlePacketForSlot(
            *slot, roomCiphers, packet, plaintext, source, localParticipant,
            encodedLocalParticipant);
    }

    [[nodiscard]] bool handlePacketForSlot(
        PeerSlot& slot,
        RoomCiphers& roomCiphers,
        std::span<const std::uint8_t> packet,
        std::array<std::uint8_t, maximumPlaintextBytes>& plaintext,
        const sockaddr_in& source,
        const PeerParticipantInfo& localParticipant,
        std::span<const std::uint8_t> encodedLocalParticipant) noexcept {
        // Reject a peer's own traffic reflected back before spending any work
        // on it. The per-direction key makes this authoritative, but checking
        // the field first keeps the rejection cheap.
        if (packet[18U] != static_cast<std::uint8_t>(slot.receiveDirection)) {
            return false;
        }
        const std::size_t payloadBytes = readU16(packet.data() + 6U);
        const auto ciphertext = packet.subspan(headerBytes, payloadBytes);
        const auto tag = std::span<const std::uint8_t, tagBytes>(
            packet.data() + headerBytes + payloadBytes, tagBytes);
        // The type is in the header, so which key opens this is decided before
        // anything is decrypted rather than after.
        const PacketType type = static_cast<PacketType>(packet[5U]);
        const auto carriedPrefix = std::span<const std::uint8_t, noncePrefixBytes>(
            packet.data() + 20U, noncePrefixBytes);
        const bool prefixMatchesSession = slot.remotePrefixKnown
            && std::memcmp(carriedPrefix.data(), slot.remoteNoncePrefix.data(),
                           noncePrefixBytes) == 0;

        // Candidates, held aside until the packet proves they are the right
        // keys. A peer that restarts arrives with a new prefix, and that has to
        // work -- but so does refusing to let a forged header tear down a
        // session that is running perfectly well.
        std::optional<AesGcmCipher> candidateSend;
        std::optional<AesGcmCipher> candidateReceive;
        AesGcmCipher* opener = nullptr;
        if (type == PacketType::Hello || type == PacketType::RoomFull) {
            // Neither can be pair-keyed. Whoever sent the join request had not
            // heard from us and could not know our prefix; whoever refused it
            // has no pair with us at all, which is the message.
            opener = &roomCiphers.forDirection(slot.receiveDirection);
        } else if (prefixMatchesSession && slot.pairReceive.has_value()) {
            opener = &*slot.pairReceive;
        } else if (buildPairCiphers(slot, carriedPrefix, candidateSend, candidateReceive)) {
            opener = &*candidateReceive;
        } else {
            return false;
        }
        if (!opener->decrypt(
                packet.first(headerBytes), ciphertext, tag,
                std::span<std::uint8_t>(plaintext.data(), payloadBytes))) {
            return false;
        }
        // Authenticated, so the prefix it carried is theirs and the keys built
        // from it are this pair's. Committed only here.
        if (type != PacketType::RoomFull
            && (!prefixMatchesSession || !slot.pairSend.has_value())) {
            // Derived straight into the slot rather than moved into it: a
            // cipher owns a platform key handle and is deliberately neither
            // copyable nor movable. This runs once when a peer arrives or
            // restarts, never per packet.
            if (buildPairCiphers(slot, carriedPrefix, slot.pairSend, slot.pairReceive)) {
                std::memcpy(
                    slot.remoteNoncePrefix.data(), carriedPrefix.data(), noncePrefixBytes);
                slot.remotePrefixKnown = true;
            }
        }
        const std::uint32_t sequence = readU32(packet.data() + 8U);
        // While this end is still finding a path, the answer legitimately comes
        // back from an address it has not settled on: a router may rewrite the
        // source, and several candidates are in flight at once. The packet has
        // already been authenticated with the room key above and the replay
        // window still applies, so accepting it grants nothing the invite did
        // not already grant. Once the session holds, the strict check returns.
        const bool negotiatingCandidates = !slot.hostRole && !slot.connected
            && (type == PacketType::HelloAck || type == PacketType::RoomFull);
        if (type != PacketType::Hello && !negotiatingCandidates
            && !sameEndpoint(source, slot.remoteAddress)) {
            return false;
        }
        const auto payload = std::span<const std::uint8_t>(
            plaintext.data(), payloadBytes);
        // A bearer invite is not identity proof. While a performer is healthy,
        // no other endpoint may repin them even if it copies that person's
        // self-reported profile ID. Secure cross-endpoint resume belongs to the
        // JL2 identity/session-token path; JL1 waits for the existing session
        // to time out before considering a replacement endpoint.
        if (type == PacketType::Hello && slot.hostRole) {
            PeerParticipantInfo proposedParticipant;
            if (!decodeParticipant(payload, proposedParticipant)
                || (slot.connected && slot.remoteAddress.sin_port != 0U
                    && !sameEndpoint(source, slot.remoteAddress))) {
                return false;
            }
        }
        // A peer that leaves and rejoins the same room restarts its nonce
        // counter from zero while this host keeps running, so its first packets
        // look far older than the window's high water mark and every one of
        // them would be rejected forever. An authenticated Hello from the
        // current endpoint (or a replacement after timeout) re-arms the window.
        if (type == PacketType::Hello && slot.hostRole) {
            slot.replayWindow.reset();
        }
        // The nonce counter is unique for every packet in this direction, so it
        // is the right anti-replay identity. Media sequences restart per stream
        // and cannot serve that purpose.
        //
        // A refusal is deliberately outside this. It is sealed under the
        // transport's own nonce context rather than the pair's, so its counter
        // has nothing to do with this slot's window and feeding it in would
        // poison the window for the session that follows.
        if (type != PacketType::RoomFull
            && !slot.replayWindow.accept(readU32(packet.data() + 20U + noncePrefixBytes))) {
            return false;
        }
        slot.lastReceive = GetTickCount64();

        if (type == PacketType::Hello && slot.hostRole) {
            PeerParticipantInfo participant;
            if (!decodeParticipant(payload, participant)) {
                return false;
            }
            slot.remoteAddress = source;
            // Once the guest's endpoint is known, keep it: if the session drops
            // and the guest's router lets its mapping expire, punching from
            // this side reopens the path without a new invite.
            slot.remoteEndpointKnown.store(true, std::memory_order_release);
            setRemoteParticipant(slot, participant);
            if (!compatibleParticipants(localParticipant, participant)) {
                slot.connected = false;
                state_.store(PeerConnectionState::VersionMismatch, std::memory_order_release);
                appendControlEvent(RoomControlEvent{
                    RoomControlEventType::VersionMismatch, 0U,
                    systemTimeMilliseconds(), participant,
                    "This room requires the exact same JamLink build"});
                static_cast<void>(sendPacket(
                    slot, roomCiphers, PacketType::VersionMismatch, 0U, 0U,
                    encodedLocalParticipant));
                return true;
            }
            const bool wasConnected = slot.connected;
            slot.connected = true;
            refreshRoomState();
            static_cast<void>(sendPacket(
                slot, roomCiphers, PacketType::HelloAck, 0U, 0U,
                encodedLocalParticipant));
            if (!wasConnected) {
                slot.sessionsEstablished.fetch_add(1U, std::memory_order_relaxed);
                appendControlEvent(RoomControlEvent{
                    RoomControlEventType::PeerJoined, 0U,
                    systemTimeMilliseconds(), participant, "joined"});
            }
            return true;
        }
        if (type == PacketType::HelloAck && !slot.hostRole) {
            PeerParticipantInfo participant;
            if (!decodeParticipant(payload, participant)) {
                return false;
            }
            // An answer is the only proof a path carries traffic both ways. A
            // packet that left proves nothing, because the far router may
            // still be dropping it. Whichever candidate answered is the one
            // audio goes to from here.
            slot.ice.onProbeResponse(candidateFromAddress(source), GetTickCount64() * 1'000ULL);
            if (!sameEndpoint(source, slot.remoteAddress)) {
                JAMLINK_LOG("ice", "path answered from a different address than probed;"
                    " adopting it");
                slot.remoteAddress = source;
            }
            setRemoteParticipant(slot, participant);
            if (!compatibleParticipants(localParticipant, participant)) {
                slot.connected = false;
                state_.store(PeerConnectionState::VersionMismatch, std::memory_order_release);
                appendControlEvent(RoomControlEvent{
                    RoomControlEventType::VersionMismatch, 0U,
                    systemTimeMilliseconds(), participant,
                    "This room requires the exact same JamLink build"});
                return true;
            }
            const bool wasConnected = slot.connected;
            slot.connected = true;
            refreshRoomState();
            if (!wasConnected) {
                slot.sessionsEstablished.fetch_add(1U, std::memory_order_relaxed);
                appendControlEvent(RoomControlEvent{
                    RoomControlEventType::PeerJoined, 0U,
                    systemTimeMilliseconds(), participant, "joined"});
            }
            return true;
        }
        // Refused. Reported rather than left to look like a network that never
        // carried us, and only while this end is still trying to get in -- a
        // stale one replayed at a musician who is already playing must not be
        // able to remove them from the room.
        if (type == PacketType::RoomFull && !slot.hostRole && !slot.connected) {
            PeerParticipantInfo participant;
            if (!decodeParticipant(payload, participant)) {
                return false;
            }
            JAMLINK_LOG("room", "the room is full and cannot take another musician");
            appendControlEvent(RoomControlEvent{
                RoomControlEventType::RoomFull, 0U,
                systemTimeMilliseconds(), participant,
                "This room is full"});
            state_.store(PeerConnectionState::RoomFull, std::memory_order_release);
            return true;
        }
        if (type == PacketType::VersionMismatch && !slot.hostRole) {
            PeerParticipantInfo participant;
            if (!decodeParticipant(payload, participant)) {
                return false;
            }
            setRemoteParticipant(slot, participant);
            slot.connected = false;
            state_.store(PeerConnectionState::VersionMismatch, std::memory_order_release);
            appendControlEvent(RoomControlEvent{
                RoomControlEventType::VersionMismatch, 0U,
                systemTimeMilliseconds(), participant,
                "This room requires the exact same JamLink build"});
            return true;
        }
        if (type == PacketType::Ping
            && (payloadBytes == sizeof(std::uint64_t)
                || payloadBytes == sizeof(std::uint64_t) + 1U
                || payloadBytes == sizeof(std::uint64_t) + 1U + audioStreamCount)) {
            if (payloadBytes == sizeof(std::uint64_t) + 1U + audioStreamCount) {
                // What this peer says it is losing of what we send. Down
                // fast and up slowly is decided in core; here it is only
                // applied, and only when the rate actually changed, so a
                // steady link never reconfigures an encoder at all.
                for (std::size_t index = 0U; index < audioStreamCount; ++index) {
                    const std::uint8_t loss =
                        plaintext[sizeof(std::uint64_t) + 1U + index];
                    if (slot.bitrateControllers[index].observe(loss)) {
                        slot.pendingBitrate[index].store(
                            slot.bitrateControllers[index].bitsPerSecond(),
                            std::memory_order_release);
                    }
                }
            }
            if (payloadBytes >= sizeof(std::uint64_t) + 1U) {
                const std::uint8_t muteMask = plaintext[sizeof(std::uint64_t)];
                for (std::size_t index = 0U; index < audioStreamCount; ++index) {
                    slot.remoteStreamMutedByPeer[index].store(
                        (muteMask & (1U << index)) != 0U ? 1U : 0U,
                        std::memory_order_release);
                }
            }
            // The Pong carries the timestamp only, so the round-trip
            // measurement is unaffected by anything added here.
            static_cast<void>(sendPacket(
                slot, roomCiphers, PacketType::Pong, 0U, 0U,
                std::span<const std::uint8_t>(plaintext.data(), sizeof(std::uint64_t))));
            return true;
        }
        if (type == PacketType::Pong && payloadBytes == sizeof(std::uint64_t)) {
            std::uint64_t sentAt = 0U;
            std::memcpy(&sentAt, plaintext.data(), sizeof(sentAt));
            const std::uint64_t now = nowMicroseconds();
            if (now >= sentAt) {
                // Against this musician alone. One number for the room would
                // be a different link every time it moved.
                slot.roundTripMicroseconds.store(
                    std::min<std::uint64_t>(now - sentAt, 60'000'000ULL),
                    std::memory_order_relaxed);
                slot.roundTripMeasured.store(true, std::memory_order_relaxed);
            }
            return true;
        }
        if (type == PacketType::Candidates && slot.connected) {
            RosterMember member;
            if (!decodeCandidateReport(payload, member)) {
                return false;
            }
            // Authenticated before it is believed. An address list is what
            // everyone else will be told to probe, so accepting an unverified
            // one would let anybody redirect a room.
            if (member.participantId != slot.remoteParticipant.profileId) {
                return false;
            }
            const std::scoped_lock lock(rosterMutex_);
            static_cast<void>(roster_.remember(member));
            rosterSize_.store(
                static_cast<std::uint32_t>(roster_.size()), std::memory_order_release);
            return true;
        }
        if (type == PacketType::Chat && slot.connected) {
            return receiveChatMessage(slot, roomCiphers, payload);
        }
        if (type == PacketType::ChatAck && slot.connected
            && payloadBytes == sizeof(std::uint64_t)) {
            receiveChatAcknowledgement(slot, readU64(plaintext.data()));
            return true;
        }
        if (type != PacketType::Audio || !slot.connected) {
            return false;
        }
        // Audio is always full 48 kHz packets of a fixed size. The sender
        // resamples before transmitting, which keeps the receive path free of
        // rate conversion and lets the jitter buffer index whole packets.
        const std::uint32_t sampleRate = readU32(packet.data() + 12U);
        const std::size_t frameCount = readU16(packet.data() + 16U);
        const std::uint8_t stream = packet[19U] & streamIndexMask;
        // The payload length now depends on the codec each packet names, so it
        // is bounded rather than fixed. The tag itself is validated by the
        // decoder, which counts anything it does not recognise instead of
        // guessing at it.
        if (sampleRate != networkSampleRate || frameCount != networkPacketFrames
            || payloadBytes <= codecTagBytes
            || payloadBytes > JamLinkStreamEncoder::maximumPacketBytes(networkPacketFrames)
            || stream >= audioStreamCount) {
            return false;
        }
        slot.remoteSourceClipped[stream].store(
            (packet[19U] & sourceClipFlag) != 0U ? 1U : 0U,
            std::memory_order_release);
        // Handed over exactly as it arrived, codec tag included, and decoded
        // at playout in sequence order. Decoding here would order a predictive
        // codec's state by arrival instead, which reordering would corrupt.
        slot.receivers[stream].submit(
            sequence,
            std::span<const std::uint8_t>(plaintext.data(), payloadBytes),
            nowMicroseconds());
        return true;
    }

    WinsockLifetime winsock_;
    SocketHandle socket_;
    std::thread worker_;
    std::array<audio::SpscAudioRing, audioStreamCount> localAudio_;
    // One per other musician. A mesh needs one fewer than the room
    // holds, since nobody sends to themselves. Created as musicians
    // arrive rather than up front, and released only when somebody
    // else needs the seat -- a slot keeps its addresses while its
    // musician is away, because those are what let a dropped session
    // re-form without a fresh invite.
    static constexpr std::size_t maximumPeerSlots =
        jamlink::control::maximumMeshParticipants - 1U;
    std::array<std::unique_ptr<PeerSlot>, maximumPeerSlots> peers_{};
    std::array<std::uint8_t, 32U> secret_{};
    std::string inviteCode_;
    // This machine as a router reported it, kept so it can be told to the
    // room rather than only put in an invite.
    std::string publicAddress_;
    std::uint16_t publicPort_{0U};
    std::uint16_t localPort_{0U};
    bool hostMode_{false};
    bool mappedPort_{false};
    std::atomic<std::uint32_t> preferredCodec_{
        static_cast<std::uint32_t>(PeerAudioCodec::Opus)};
    std::atomic<std::uint64_t> encodeFailures_{0U};
    mutable std::mutex controlMutex_;
    PeerParticipantInfo localParticipant_{
        "local-development", "", "Musician", "avatar:guitar-electric",
        "Guitar", "development", "development", "test"};
    std::deque<PendingChat> pendingChat_;
    std::deque<RoomControlEvent> controlEvents_;
    std::deque<ULONGLONG> outboundChatTimes_;
    std::deque<ULONGLONG> inboundChatTimes_;
    std::array<std::uint64_t, maximumRememberedChatIds> receivedChatIds_{};
    std::size_t receivedChatCount_{0U};
    std::size_t receivedChatCursor_{0U};
    std::uint64_t nextChatMessageId_{1U};
    // The nonce context a refusal is sent under. Separate from every slot's,
    // because a refusal is sent precisely when no slot was available, and
    // borrowing one would reuse that slot's nonce under the room key.
    std::array<std::uint8_t, noncePrefixBytes> refusalNoncePrefix_{};
    std::uint32_t refusalNonceCounter_{0U};
    bool refusalNonceExhausted_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<PeerConnectionState> state_{PeerConnectionState::Idle};
    std::atomic<std::uint32_t> sendMuted_{0U};
    std::array<std::atomic<std::uint32_t>, audioStreamCount> localStreamMuted_{};
    std::array<std::atomic<std::uint32_t>, audioStreamCount> localSourceClipped_{};
    // Each capture device has its own rate: an ASIO interface for the guitar
    // and a USB microphone for the voice need not agree, and forcing one rate
    // on both would resample whichever was wrong.
    std::array<std::atomic<std::uint32_t>, audioStreamCount> localSampleRate_{};
    // What the listening controls currently say. Held here as well as on each
    // slot so a musician who joins after somebody turned the voice stream
    // down arrives at that setting rather than at full volume.
    // Owned by the audio callback, which is the only thread that mixes.
    std::array<float, 1'024U> mixScratch_{};
    std::array<float, audioStreamCount> remoteGain_{1.0F, 1.0F};
    std::array<bool, audioStreamCount> remoteMuted_{};
    std::size_t latencyMinimumPackets_{2U};
    std::size_t latencyMaximumPackets_{32U};
    double latencySafety_{2.5};
    // Who is in the room and where they can be reached. Guarded because the
    // worker fills it in while the control thread reads it for the interface.
    mutable std::mutex rosterMutex_;
    RoomRoster roster_;
    // How many the roster names, published for the noexcept telemetry call.
    std::atomic<std::uint32_t> rosterSize_{0U};
    std::atomic<std::uint64_t> packetsSent_{0U};
    // Owned by the network worker, which is the only thread that encodes.
    std::array<jamlink::audio::SendLimiter, audioStreamCount> sendLimiters_{};
    std::atomic<std::uint32_t> iceRounds_{0U};
    std::atomic<std::uint64_t> iceProbes_{0U};
    std::atomic<std::uint64_t> packetsReceived_{0U};
    std::atomic<std::uint64_t> packetsRejected_{0U};
    std::atomic<bool> automaticPortMapping_{false};
    // Which of the three mapping protocols opened the port, for the preflight
    // to report. Empty when none of them did.
    const char* gatewayMappingProtocol_{""};
    std::atomic<bool> udpBound_{false};
    std::atomic<NatMappingBehaviour> natBehaviour_{NatMappingBehaviour::NotProbed};
    std::atomic<PublicAddressDiscoveryState> publicAddressDiscovery_{
        PublicAddressDiscoveryState::NotAttempted};
    std::atomic<PortMappingState> portMapping_{PortMappingState::NotRequested};
    std::atomic<ReachabilityAssessment> reachability_{ReachabilityAssessment::Unknown};
};

} // namespace

std::unique_ptr<IPeerAudioTransport> createPlatformPeerAudioTransport() {
    return std::make_unique<WindowsPeerAudioTransport>();
}

} // namespace jamlink::network
