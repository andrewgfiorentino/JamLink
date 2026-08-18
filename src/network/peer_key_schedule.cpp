// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/peer_key_schedule.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace jamlink::network {
namespace {

HmacSha256Function hmac = nullptr;

// Distinct labels per direction, so a packet cannot be reflected back at its
// sender and still authenticate. The pair discriminator is appended rather
// than mixed into the secret: one HMAC invocation with a structured message is
// HKDF-Expand with a single output block, not a bespoke construction.
constexpr char hostToGuestLabel[] = "JamLink JL2 pair host-to-guest audio key";
constexpr char guestToHostLabel[] = "JamLink JL2 pair guest-to-host audio key";

} // namespace

void installHmacSha256(HmacSha256Function function) noexcept { hmac = function; }

bool hmacAvailable() noexcept { return hmac != nullptr; }

std::array<std::uint8_t, 2U * peerNoncePrefixBytes> pairDiscriminator(
    std::span<const std::uint8_t, peerNoncePrefixBytes> first,
    std::span<const std::uint8_t, peerNoncePrefixBytes> second) noexcept {
    std::array<std::uint8_t, 2U * peerNoncePrefixBytes> result{};
    // Sorted, so both ends of a pair compute the same value without having to
    // agree which of them is "first". Without this each side would derive a
    // different key from the same two prefixes and nothing would decrypt.
    const bool firstIsLower = std::lexicographical_compare(
        first.begin(), first.end(), second.begin(), second.end());
    const auto& lower = firstIsLower ? first : second;
    const auto& higher = firstIsLower ? second : first;
    std::memcpy(result.data(), lower.data(), peerNoncePrefixBytes);
    std::memcpy(result.data() + peerNoncePrefixBytes, higher.data(), peerNoncePrefixBytes);
    return result;
}

bool derivePeerKey(
    std::span<const std::uint8_t, 32U> roomSecret,
    std::span<const std::uint8_t, peerNoncePrefixBytes> localNoncePrefix,
    std::span<const std::uint8_t, peerNoncePrefixBytes> remoteNoncePrefix,
    KeyDirection direction,
    std::span<std::uint8_t, 32U> key) noexcept {
    if (hmac == nullptr) {
        // Fails closed. Sending in the clear because key derivation was
        // unavailable would be the worst possible recovery, so there is no
        // fallback path here at all.
        return false;
    }
    const auto discriminator = pairDiscriminator(localNoncePrefix, remoteNoncePrefix);
    const char* label = direction == KeyDirection::HostToGuest
        ? hostToGuestLabel : guestToHostLabel;
    const std::size_t labelBytes = direction == KeyDirection::HostToGuest
        ? sizeof(hostToGuestLabel) - 1U
        : sizeof(guestToHostLabel) - 1U;

    std::array<std::uint8_t, sizeof(hostToGuestLabel) + 2U * peerNoncePrefixBytes> message{};
    std::memcpy(message.data(), label, labelBytes);
    std::memcpy(message.data() + labelBytes, discriminator.data(), discriminator.size());
    return hmac(
        roomSecret,
        std::span<const std::uint8_t>(message.data(), labelBytes + discriminator.size()),
        key);
}

} // namespace jamlink::network
