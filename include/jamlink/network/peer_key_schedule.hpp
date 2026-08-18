// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace jamlink::network {

// Keys that belong to a pair of musicians rather than to the room.
//
// Today both directions are keyed from the room secret alone. With one peer
// that is sound. With a mesh it is two separate defects:
//
//   Everyone in the room can read everyone else's traffic. Two people's chat
//   and audio would be decryptable by a third who was merely in the same room,
//   which is not what a private session means.
//
//   Worse, a packet is not bound to its recipient. Sealed under a key everyone
//   holds, a packet addressed to one musician authenticates perfectly as one
//   addressed to another. Cross-peer confusion of that kind is a security
//   defect rather than a glitch, and it is invisible while there is only ever
//   one peer to be confused with.
//
// The fix is that every pair derives its own keys. The difficulty is finding
// something both ends of a pair agree on before either can decrypt anything
// the other sent -- a chicken and egg that a naive design solves by
// renegotiating, which costs a round trip and a rekey point.
//
// It does not need one. Every packet already carries the sender's nonce prefix
// in its header, in the clear, because the receiver needs it to reconstruct the
// nonce. It is eight random bytes chosen per session per side. Both ends
// therefore hold both prefixes from the first packet onward, without anything
// new on the wire.
//
// Sorting the two prefixes makes the pair identity symmetric: each side
// computes the same discriminator without having to agree who is "first".
// Direction is then layered on top, exactly as it is now, so a packet still
// cannot be reflected back at its sender.
inline constexpr std::size_t peerNoncePrefixBytes = 8U;

enum class KeyDirection : std::uint8_t { HostToGuest, GuestToHost };

// Derives one direction's key for one pair.
//
// Returns false only if the platform HMAC is unavailable, which is a fatal
// condition the caller must surface rather than continue past: sending
// unencrypted because key derivation failed would be the worst possible
// recovery.
[[nodiscard]] bool derivePeerKey(
    std::span<const std::uint8_t, 32U> roomSecret,
    std::span<const std::uint8_t, peerNoncePrefixBytes> localNoncePrefix,
    std::span<const std::uint8_t, peerNoncePrefixBytes> remoteNoncePrefix,
    KeyDirection direction,
    std::span<std::uint8_t, 32U> key) noexcept;

// The pair discriminator on its own, exposed for testing and for diagnostics
// that need to say two peers are keyed apart without revealing either key.
[[nodiscard]] std::array<std::uint8_t, 2U * peerNoncePrefixBytes> pairDiscriminator(
    std::span<const std::uint8_t, peerNoncePrefixBytes> first,
    std::span<const std::uint8_t, peerNoncePrefixBytes> second) noexcept;

// The HMAC-SHA256 this is built on. Supplied by the platform so that core stays
// free of any particular crypto library; the transport installs it once at
// startup. Absent, derivation fails closed rather than falling back to
// something weaker.
using HmacSha256Function = bool (*)(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> message,
    std::span<std::uint8_t, 32U> digest);

void installHmacSha256(HmacSha256Function function) noexcept;
[[nodiscard]] bool hmacAvailable() noexcept;

} // namespace jamlink::network
