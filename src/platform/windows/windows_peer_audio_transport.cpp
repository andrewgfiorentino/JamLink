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
#include "jamlink/audio/gain_stage.hpp"
#include "jamlink/audio/realtime_atomic.hpp"
#include "jamlink/audio/spsc_audio_ring.hpp"
#include "jamlink/network/audio_stream_receiver.hpp"
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
#include <memory>
#include <span>
#include <string>
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
constexpr std::uint8_t protocolVersion = 2U;
constexpr std::uint32_t networkSampleRate = 48'000U;
constexpr std::size_t networkPacketFrames = 240U;
constexpr std::size_t noncePrefixBytes = 8U;
constexpr std::uint32_t maximumNonceCounter = 0xFFFFFF00U;

enum class PacketType : std::uint8_t {
    Hello = 1U,
    HelloAck = 2U,
    Audio = 3U,
    Ping = 4U,
    Pong = 5U
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

[[nodiscard]] StunEndpoint queryStun(SOCKET socket) {
    StunEndpoint result;
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo("stun.cloudflare.com", "3478", &hints, &addresses) != 0) {
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

class WindowsPeerAudioTransport final : public IPeerAudioTransport {
public:
    WindowsPeerAudioTransport()
        : localAudio_{
              audio::SpscAudioRing(32'768U, 1U),
              audio::SpscAudioRing(32'768U, 1U)},
          receivers_{
              AudioStreamReceiver(receiverSettings()),
              AudioStreamReceiver(receiverSettings())} {}
    ~WindowsPeerAudioTransport() override { stop(); }

    [[nodiscard]] std::string host(
        std::uint16_t port,
        bool prepareInternetReachability) override {
        stop();
        if (!winsock_.available() || !createBoundSocket(port)) {
            state_.store(PeerConnectionState::SocketFailed, std::memory_order_release);
            return {};
        }
        if (!BCRYPT_SUCCESS(BCryptGenRandom(
                nullptr, secret_.data(), static_cast<ULONG>(secret_.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
            socket_.reset();
            return {};
        }

        const std::string localAddress = localIpv4Address();
        const PortMappingResult mapping = prepareInternetReachability
            ? addAutomaticPortMapping(localPort_, localAddress)
            : PortMappingResult{};
        mappedPort_ = mapping.mapped;
        const StunEndpoint publicEndpoint = prepareInternetReachability
            ? queryStun(socket_.get())
            : StunEndpoint{};
        const bool usableAutomaticMapping = mapping.mapped
            && (publicEndpoint.succeeded || !mapping.externalAddress.empty());
        automaticPortMapping_.store(usableAutomaticMapping, std::memory_order_relaxed);
        std::string inviteAddress;
        if (publicEndpoint.succeeded) {
            inviteAddress = publicEndpoint.address;
        } else if (!mapping.externalAddress.empty()) {
            inviteAddress = mapping.externalAddress;
        } else {
            inviteAddress = localAddress;
        }
        // UPnP requests the same external and internal UDP port. Keeping that
        // port in the fallback invite also makes manual forwarding explicit
        // and deterministic when automatic mapping is unavailable.
        inviteCode_ = "JL1|" + inviteAddress + "|" + std::to_string(localPort_)
            + "|" + hexEncode(secret_);
        hostMode_ = true;
        state_.store(PeerConnectionState::WaitingForPeer, std::memory_order_release);
        launchWorker();
        return inviteCode_;
    }

    [[nodiscard]] bool join(const std::string& inviteCode) override {
        stop();
        std::string address;
        std::uint16_t port = 0U;
        if (!parseInvite(inviteCode, address, port, secret_)) {
            state_.store(PeerConnectionState::InviteInvalid, std::memory_order_release);
            return false;
        }
        if (!winsock_.available() || !createBoundSocket(0U)) {
            state_.store(PeerConnectionState::SocketFailed, std::memory_order_release);
            return false;
        }
        remoteAddress_ = {};
        remoteAddress_.sin_family = AF_INET;
        remoteAddress_.sin_port = htons(port);
        if (inet_pton(AF_INET, address.c_str(), &remoteAddress_.sin_addr) != 1) {
            state_.store(PeerConnectionState::InviteInvalid, std::memory_order_release);
            socket_.reset();
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
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            localAudio_[index].clear();
            receivers_[index].reset();
            remotePeak_[index].store(0.0F);
            sendSequence_[index] = 0U;
        }
        state_.store(PeerConnectionState::Idle, std::memory_order_release);
    }

    void setSendMuted(bool muted) noexcept override {
        sendMuted_.store(muted ? 1U : 0U, std::memory_order_release);
    }

    void setLocalStreamMuted(AudioStreamId stream, bool muted) noexcept override {
        localStreamMuted_[streamIndex(stream)].store(
            muted ? 1U : 0U, std::memory_order_release);
    }

    void setRemoteStreamGain(AudioStreamId stream, float gain) noexcept override {
        remoteGain_[streamIndex(stream)].setLinearGain(gain);
    }

    void setRemoteStreamMuted(AudioStreamId stream, bool muted) noexcept override {
        remoteGain_[streamIndex(stream)].setMuted(muted);
    }

    [[nodiscard]] std::string inviteCode() const override { return inviteCode_; }
    [[nodiscard]] std::uint16_t localPort() const noexcept override { return localPort_; }

    [[nodiscard]] PeerTransportTelemetry telemetry() const noexcept override {
        PeerTransportTelemetry snapshot;
        snapshot.state = state_.load(std::memory_order_acquire);
        snapshot.packetsSent = packetsSent_.load(std::memory_order_relaxed);
        snapshot.packetsReceived = packetsReceived_.load(std::memory_order_relaxed);
        snapshot.packetsRejected = packetsRejected_.load(std::memory_order_relaxed);
        snapshot.roundTripMicroseconds =
            roundTripMicroseconds_.load(std::memory_order_relaxed);
        snapshot.automaticPortMapping =
            automaticPortMapping_.load(std::memory_order_relaxed);
        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            snapshot.localAudioDrops += localAudio_[index].overrunCount();
            const auto receiver = receivers_[index].telemetry();
            RemoteStreamTelemetry& stream = snapshot.streams[index];
            stream.peak = remotePeak_[index].load();
            stream.packetsConcealed = receiver.packetsConcealed;
            stream.packetsLate = receiver.packetsLate;
            stream.bufferStretches = receiver.bufferStretches;
            stream.latencyTrims = receiver.latencyTrims;
            stream.jitterMicroseconds = receiver.jitterMicroseconds;
            stream.bufferedFrames = receiver.currentDepthFrames;
            stream.targetFrames = receiver.targetDepthFrames;
            stream.playing = receiver.playing;
        }
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
        localSampleRate_.store(sampleRate, std::memory_order_relaxed);
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
        const std::size_t live = receivers_[index].pull(monoSamples);
        remoteGain_[index].process(
            audio::InterleavedAudioBlock{monoSamples, 1U});
        float peak = 0.0F;
        for (const float sample : monoSamples) {
            peak = std::max(peak, std::abs(sample));
        }
        remotePeak_[index].store(std::clamp(peak, 0.0F, 1.0F));
        return live;
    }

private:
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
        packetsReceived_.store(0U, std::memory_order_relaxed);
        packetsRejected_.store(0U, std::memory_order_relaxed);
        roundTripMicroseconds_.store(0U, std::memory_order_relaxed);
        for (auto& peak : remotePeak_) {
            peak.store(0.0F);
        }
        worker_ = std::thread([this] { run(); });
    }

    [[nodiscard]] std::size_t buildEncryptedPacket(
        AesGcmCipher& cipher,
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
        destination[18U] = static_cast<std::uint8_t>(sendDirection_);
        destination[19U] = stream;
        std::memcpy(destination.data() + 20U, noncePrefix_.data(), noncePrefixBytes);
        const std::uint32_t nonceCounter = ++nonceCounter_;
        if (nonceCounter >= maximumNonceCounter) {
            // Never reuse a nonce. Refusing to send is the only safe response.
            return 0U;
        }
        writeU32(destination.data() + 20U + noncePrefixBytes, nonceCounter);
        auto ciphertext = destination.subspan(headerBytes, plaintext.size());
        auto tag = std::span<std::uint8_t, tagBytes>(
            destination.data() + headerBytes + plaintext.size(), tagBytes);
        if (!cipher.encrypt(destination.first(headerBytes), plaintext, ciphertext, tag)) {
            return 0U;
        }
        return headerBytes + plaintext.size() + tagBytes;
    }

    [[nodiscard]] bool sendPacket(
        AesGcmCipher& cipher,
        PacketType type,
        std::uint32_t sampleRate,
        std::uint16_t frameCount,
        std::span<const std::uint8_t> plaintext,
        AudioStreamId stream = AudioStreamId::Instrument) noexcept {
        std::array<std::uint8_t, maximumDatagramBytes> packet{};
        // Media sequences are per stream so each receive buffer sees a
        // contiguous run. Replay protection uses the nonce counter, which is
        // unique across every packet in this direction.
        const std::uint32_t sequence = type == PacketType::Audio
            ? ++sendSequence_[streamIndex(stream)]
            : 0U;
        const std::size_t bytes = buildEncryptedPacket(
            cipher, type, sequence, sampleRate, frameCount,
            type == PacketType::Audio ? static_cast<std::uint8_t>(stream) : std::uint8_t{0},
            plaintext, packet);
        if (bytes == 0U) {
            return false;
        }
        const int sent = sendto(
            socket_.get(), reinterpret_cast<const char*>(packet.data()),
            static_cast<int>(bytes), 0,
            reinterpret_cast<const sockaddr*>(&remoteAddress_), sizeof(remoteAddress_));
        if (sent == static_cast<int>(bytes)) {
            packetsSent_.fetch_add(1U, std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    void run() noexcept {
        try {
            sendDirection_ = hostMode_ ? Direction::HostToGuest : Direction::GuestToHost;
            receiveDirection_ = hostMode_ ? Direction::GuestToHost : Direction::HostToGuest;
            std::array<std::uint8_t, 32U> sendKey{};
            std::array<std::uint8_t, 32U> receiveKey{};
            if (!deriveDirectionKey(secret_, sendDirection_, sendKey)
                || !deriveDirectionKey(secret_, receiveDirection_, receiveKey)) {
                state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
                return;
            }
            AesGcmCipher sendCipher(sendKey);
            AesGcmCipher receiveCipher(receiveKey);
            SecureZeroMemory(sendKey.data(), sendKey.size());
            SecureZeroMemory(receiveKey.data(), receiveKey.size());
            replayWindow_.reset();
            nonceCounter_ = 0U;
            if (!sendCipher.valid() || !receiveCipher.valid()
                || !BCRYPT_SUCCESS(BCryptGenRandom(
                    nullptr, noncePrefix_.data(), static_cast<ULONG>(noncePrefix_.size()),
                    BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
                state_.store(PeerConnectionState::EncryptionFailed, std::memory_order_release);
                return;
            }
            std::array<audio::AsyncMonoResampler, audioStreamCount> outgoingResamplers{
                audio::AsyncMonoResampler(65'536U),
                audio::AsyncMonoResampler(65'536U)};
            std::uint32_t outgoingRate = networkSampleRate;
            for (auto& resampler : outgoingResamplers) {
                resampler.configure(outgoingRate, networkSampleRate);
            }
            std::array<float, 1'024U> localScratch{};
            std::array<float, networkPacketFrames> networkFloat{};
            std::array<std::uint8_t, networkPacketFrames * 2U> networkPcm{};
            std::array<std::uint8_t, maximumDatagramBytes> receivedPacket{};
            std::array<std::uint8_t, maximumPlaintextBytes> decrypted{};
            std::array<std::uint8_t, 8U> controlPayload{};
            ULONGLONG lastHello = 0U;
            ULONGLONG lastPing = 0U;
            ULONGLONG lastReceive = GetTickCount64();
            bool connected = false;

            while (!stopRequested_.load(std::memory_order_acquire)) {
                const ULONGLONG now = GetTickCount64();
                if (!hostMode_ && !connected && now - lastHello >= 250U) {
                    constexpr std::array<std::uint8_t, 4U> hello{{'J', 'O', 'I', 'N'}};
                    static_cast<void>(sendPacket(
                        sendCipher, PacketType::Hello, 0U, 0U, hello));
                    lastHello = now;
                }
                if (connected && now - lastPing >= 500U) {
                    const std::uint64_t stamp = nowMicroseconds();
                    std::memcpy(controlPayload.data(), &stamp, sizeof(stamp));
                    static_cast<void>(sendPacket(
                        sendCipher, PacketType::Ping, 0U, 0U, controlPayload));
                    lastPing = now;
                }

                drainOutgoingAudio(
                    sendCipher, outgoingResamplers, outgoingRate,
                    localScratch, networkFloat, networkPcm, connected);

                fd_set readSet;
                FD_ZERO(&readSet);
                FD_SET(socket_.get(), &readSet);
                timeval timeout{0, 5'000};
                const int selected = select(0, &readSet, nullptr, nullptr, &timeout);
                if (selected > 0 && FD_ISSET(socket_.get(), &readSet)) {
                    for (;;) {
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
                                sendCipher, receiveCipher,
                                std::span<const std::uint8_t>(receivedPacket.data(),
                                    static_cast<std::size_t>(bytes)),
                                decrypted, source, connected, lastReceive,
                                networkFloat)) {
                            packetsReceived_.fetch_add(1U, std::memory_order_relaxed);
                        } else {
                            packetsRejected_.fetch_add(1U, std::memory_order_relaxed);
                        }
                    }
                }
                if (connected && GetTickCount64() - lastReceive > 5'000U) {
                    connected = false;
                    state_.store(
                        hostMode_ ? PeerConnectionState::WaitingForPeer
                                  : PeerConnectionState::ConnectionLost,
                        std::memory_order_release);
                    if (!hostMode_) {
                        state_.store(PeerConnectionState::Connecting, std::memory_order_release);
                    }
                }
            }
        } catch (...) {
            state_.store(PeerConnectionState::SocketFailed, std::memory_order_release);
        }
    }

    void drainOutgoingAudio(
        AesGcmCipher& cipher,
        std::array<audio::AsyncMonoResampler, audioStreamCount>& resamplers,
        std::uint32_t& configuredRate,
        std::array<float, 1'024U>& localScratch,
        std::array<float, networkPacketFrames>& networkFloat,
        std::array<std::uint8_t, networkPacketFrames * 2U>& networkPcm,
        bool connected) noexcept {
        if (!connected) {
            return;
        }
        const std::uint32_t requestedRate = localSampleRate_.load(std::memory_order_relaxed);
        if (requestedRate != configuredRate && requestedRate >= 8'000U
            && requestedRate <= 384'000U) {
            configuredRate = requestedRate;
            for (auto& resampler : resamplers) {
                resampler.configure(configuredRate, networkSampleRate);
            }
        }

        for (std::size_t index = 0U; index < audioStreamCount; ++index) {
            auto& resampler = resamplers[index];
            while (localAudio_[index].availableReadFrames() > 0U) {
                const std::size_t frames = std::min(
                    localScratch.size(), localAudio_[index].availableReadFrames());
                static_cast<void>(localAudio_[index].readAndZeroFill(
                    std::span<float>(localScratch.data(), frames)));
                static_cast<void>(resampler.write(
                    std::span<const float>(localScratch.data(), frames)));
            }
            for (std::size_t packet = 0U; packet < 4U; ++packet) {
                if (resampler.read(networkFloat) != networkFloat.size()) {
                    break;
                }
                for (std::size_t frame = 0U; frame < networkFloat.size(); ++frame) {
                    const float bounded = std::clamp(networkFloat[frame], -0.98F, 0.98F);
                    const auto sample =
                        static_cast<std::int16_t>(std::lrint(bounded * 32'767.0F));
                    networkPcm[frame * 2U] = static_cast<std::uint8_t>(sample & 0xFF);
                    networkPcm[frame * 2U + 1U] =
                        static_cast<std::uint8_t>((sample >> 8) & 0xFF);
                }
                static_cast<void>(sendPacket(
                    cipher, PacketType::Audio, networkSampleRate,
                    static_cast<std::uint16_t>(networkPacketFrames), networkPcm,
                    static_cast<AudioStreamId>(index)));
            }
        }
    }

    [[nodiscard]] bool handlePacket(
        AesGcmCipher& sendCipher,
        AesGcmCipher& receiveCipher,
        std::span<const std::uint8_t> packet,
        std::array<std::uint8_t, maximumPlaintextBytes>& plaintext,
        const sockaddr_in& source,
        bool& connected,
        ULONGLONG& lastReceive,
        std::array<float, networkPacketFrames>& networkFloat) noexcept {
        if (packet.size() < headerBytes + tagBytes
            || readU32(packet.data()) != protocolMagic
            || packet[4U] != protocolVersion
            // Reject a peer's own traffic reflected back before spending any
            // work on it. The per-direction key makes this authoritative, but
            // checking the field first keeps the rejection cheap.
            || packet[18U] != static_cast<std::uint8_t>(receiveDirection_)
            || packet[19U] >= audioStreamCount) {
            return false;
        }
        const std::size_t payloadBytes = readU16(packet.data() + 6U);
        if (payloadBytes > plaintext.size()
            || packet.size() != headerBytes + payloadBytes + tagBytes) {
            return false;
        }
        const auto ciphertext = packet.subspan(headerBytes, payloadBytes);
        const auto tag = std::span<const std::uint8_t, tagBytes>(
            packet.data() + headerBytes + payloadBytes, tagBytes);
        if (!receiveCipher.decrypt(
                packet.first(headerBytes), ciphertext, tag,
                std::span<std::uint8_t>(plaintext.data(), payloadBytes))) {
            return false;
        }
        const PacketType type = static_cast<PacketType>(packet[5U]);
        const std::uint32_t sequence = readU32(packet.data() + 8U);
        if (type != PacketType::Hello && !sameEndpoint(source, remoteAddress_)) {
            return false;
        }
        // The nonce counter is unique for every packet in this direction, so it
        // is the right anti-replay identity. Media sequences restart per stream
        // and cannot serve that purpose.
        if (!replayWindow_.accept(readU32(packet.data() + 20U + noncePrefixBytes))) {
            return false;
        }
        lastReceive = GetTickCount64();

        if (type == PacketType::Hello && hostMode_) {
            remoteAddress_ = source;
            connected = true;
            state_.store(PeerConnectionState::Connected, std::memory_order_release);
            constexpr std::array<std::uint8_t, 2U> ack{{'O', 'K'}};
            static_cast<void>(sendPacket(sendCipher, PacketType::HelloAck, 0U, 0U, ack));
            return true;
        }
        if (type == PacketType::HelloAck && !hostMode_) {
            connected = true;
            state_.store(PeerConnectionState::Connected, std::memory_order_release);
            return true;
        }
        if (type == PacketType::Ping && payloadBytes == sizeof(std::uint64_t)) {
            static_cast<void>(sendPacket(
                sendCipher, PacketType::Pong, 0U, 0U,
                std::span<const std::uint8_t>(plaintext.data(), payloadBytes)));
            return true;
        }
        if (type == PacketType::Pong && payloadBytes == sizeof(std::uint64_t)) {
            std::uint64_t sentAt = 0U;
            std::memcpy(&sentAt, plaintext.data(), sizeof(sentAt));
            const std::uint64_t now = nowMicroseconds();
            if (now >= sentAt) {
                roundTripMicroseconds_.store(
                    std::min<std::uint64_t>(now - sentAt, 60'000'000ULL),
                    std::memory_order_relaxed);
            }
            return true;
        }
        if (type != PacketType::Audio || !connected) {
            return false;
        }
        // Audio is always full 48 kHz packets of a fixed size. The sender
        // resamples before transmitting, which keeps the receive path free of
        // rate conversion and lets the jitter buffer index whole packets.
        const std::uint32_t sampleRate = readU32(packet.data() + 12U);
        const std::size_t frameCount = readU16(packet.data() + 16U);
        const std::uint8_t stream = packet[19U];
        if (sampleRate != networkSampleRate || frameCount != networkPacketFrames
            || payloadBytes != frameCount * 2U
            || stream >= audioStreamCount) {
            return false;
        }
        for (std::size_t frame = 0U; frame < frameCount; ++frame) {
            const auto sample = static_cast<std::int16_t>(
                static_cast<std::uint16_t>(plaintext[frame * 2U])
                | (static_cast<std::uint16_t>(plaintext[frame * 2U + 1U]) << 8U));
            networkFloat[frame] = static_cast<float>(sample) / 32'768.0F;
        }
        receivers_[stream].submit(
            sequence,
            std::span<const float>(networkFloat.data(), frameCount),
            nowMicroseconds());
        return true;
    }

    WinsockLifetime winsock_;
    SocketHandle socket_;
    std::thread worker_;
    std::array<audio::SpscAudioRing, audioStreamCount> localAudio_;
    std::array<AudioStreamReceiver, audioStreamCount> receivers_;
    std::array<audio::GainStage, audioStreamCount> remoteGain_{
        audio::GainStage(1.0F), audio::GainStage(1.0F)};
    std::array<std::uint8_t, 32U> secret_{};
    std::array<std::uint8_t, noncePrefixBytes> noncePrefix_{};
    sockaddr_in remoteAddress_{};
    std::string inviteCode_;
    std::uint16_t localPort_{0U};
    bool hostMode_{false};
    bool mappedPort_{false};
    Direction sendDirection_{Direction::HostToGuest};
    Direction receiveDirection_{Direction::GuestToHost};
    ReplayWindow replayWindow_;
    std::uint32_t nonceCounter_{0U};
    std::array<std::uint32_t, audioStreamCount> sendSequence_{};
    std::atomic<bool> stopRequested_{false};
    std::atomic<PeerConnectionState> state_{PeerConnectionState::Idle};
    std::atomic<std::uint32_t> sendMuted_{0U};
    std::array<std::atomic<std::uint32_t>, audioStreamCount> localStreamMuted_{};
    std::atomic<std::uint32_t> localSampleRate_{networkSampleRate};
    std::atomic<std::uint64_t> packetsSent_{0U};
    std::atomic<std::uint64_t> packetsReceived_{0U};
    std::atomic<std::uint64_t> packetsRejected_{0U};
    std::atomic<std::uint64_t> roundTripMicroseconds_{0U};
    std::atomic<bool> automaticPortMapping_{false};
    std::array<audio::RealtimeAtomicFloat, audioStreamCount> remotePeak_{
        audio::RealtimeAtomicFloat(0.0F), audio::RealtimeAtomicFloat(0.0F)};
};

} // namespace

std::unique_ptr<IPeerAudioTransport> createPlatformPeerAudioTransport() {
    return std::make_unique<WindowsPeerAudioTransport>();
}

} // namespace jamlink::network
