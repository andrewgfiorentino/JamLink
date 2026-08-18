// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace jamlink::network {

// What a router does to this machine's address on the way out, and what that
// means for a musician trying to host.
//
// A router that gives the same external port to every destination is one a
// friend can be sent to: the port in an invite is the port their packets will
// arrive on. A router that picks a fresh port per destination -- symmetric --
// hands out a port that was only ever valid for the STUN server that observed
// it, so an invite built from it names an address nobody can reach.
//
// Field testing produced exactly that failure repeatedly: a public address
// discovered, an invite that looked correct, and no connection. Nothing in the
// application could tell the difference between that and a slow start, so the
// musician was left watching a spinner. The one thing they needed to be told
// was that this machine cannot host and the other person should.
//
// The classification is a pure function of two observations so it can be
// tested without a network. Deciding it from real routers only, between two
// houses, is how it went untested and unreported for as long as it did.
enum class NatMappingBehaviour : std::uint8_t {
    // No usable observation. Never presented as a verdict.
    NotProbed,
    // Only one server answered. A single observation cannot distinguish the
    // two behaviours, and guessing here would be worse than saying nothing:
    // wrongly claiming symmetric sends a musician to ask their friend to host
    // when they did not need to.
    Inconclusive,
    // The same external address and port to every destination. Hosting works.
    EndpointIndependent,
    // A different external port, or address, per destination. Symmetric. A
    // direct invite from this machine cannot work.
    AddressOrPortDependent,
};

[[nodiscard]] std::string_view natBehaviourName(NatMappingBehaviour behaviour) noexcept;

// One STUN server's view of this socket.
struct ObservedMapping final {
    bool observed{false};
    std::string address;
    std::uint16_t port{0U};
};

struct NatAssessment final {
    NatMappingBehaviour behaviour{NatMappingBehaviour::NotProbed};
    // Whether an invite made on this machine names an endpoint a friend can
    // actually reach. False only when the answer is known, never when it is
    // merely unproven -- an unproven case must not block a musician who would
    // have connected perfectly well.
    [[nodiscard]] bool canHostDirectly() const noexcept {
        return behaviour != NatMappingBehaviour::AddressOrPortDependent;
    }
    // True when the two observations disagreed about the port specifically,
    // which is the ordinary symmetric case and the one worth naming in a log.
    bool portRewritten{false};

    // Musician language. Empty when there is nothing worth saying, so the
    // interface can decide whether to show anything from whether this is empty.
    [[nodiscard]] std::string_view advice() const noexcept;
};

// Two observations of the same socket, taken from two different servers.
//
// They have to be different servers: asking one server twice measures nothing,
// because a symmetric router keeps the same port for the same destination and
// would look endpoint-independent every time.
[[nodiscard]] NatAssessment classifyNatBehaviour(
    const ObservedMapping& first, const ObservedMapping& second) noexcept;

} // namespace jamlink::network
