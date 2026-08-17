// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace jamlink::network {

// Finding a path between two homes without asking either router's permission.
//
// What JamLink does today is ask the router to open a port, put one address in
// the invite, and have the guest connect to it. That works when the router
// cooperates. Field testing has repeatedly shown it refusing: UPnP granted at
// one hour and refused the next on the same router, and nothing to fall back on.
//
// Hole punching does not ask. Both ends send to each other at the same moment,
// and each outbound packet creates the very mapping the other end's packet
// needs. It works through most home NATs precisely because it never requires
// an inbound connection to arrive unannounced.
//
// Two things make it work that the current code does not do:
//
//   Both sides must punch. A packet from one side only opens one router. The
//   other router still drops the incoming packet because nothing went out
//   through it first.
//
//   There is rarely one address. A machine has a LAN address, a public address
//   as seen by a STUN server, and possibly more. The pair that works cannot be
//   known in advance, so every plausible pair is probed and the first that
//   answers is used.
//
// This agent owns that decision and nothing else. It holds no socket and no
// clock: it is told what time it is and what has arrived, and it says what to
// send next. That keeps a notoriously timing-dependent negotiation reproducible
// in a test rather than only observable between two houses.
enum class CandidateKind : std::uint8_t {
    // An address on this machine's own network. When two musicians are in the
    // same building this pair wins outright and never leaves the LAN.
    Host,
    // This machine as a STUN server saw it: the public address and the port the
    // router chose. This is the pair that works between two homes.
    ServerReflexive,
    // Supplied by a relay. Always works, always costs an extra hop, so it is
    // tried last and only if everything else fails.
    Relayed,
};

struct IceCandidate final {
    std::string address;
    std::uint16_t port{0U};
    CandidateKind kind{CandidateKind::Host};

    [[nodiscard]] bool operator==(const IceCandidate& other) const noexcept {
        return port == other.port && kind == other.kind && address == other.address;
    }
    [[nodiscard]] bool valid() const noexcept { return !address.empty() && port != 0U; }
};

// One local candidate paired with one remote candidate, and what probing it has
// found out so far.
enum class PairState : std::uint8_t {
    Waiting,
    Probing,
    // A probe from this pair came back, so the path carries traffic both ways.
    Succeeded,
    // Enough probes went unanswered that this pair is not worth more time.
    Failed,
};

struct CandidatePair final {
    IceCandidate local;
    IceCandidate remote;
    PairState state{PairState::Waiting};
    std::uint32_t probesSent{0U};
    std::uint64_t firstProbeMicroseconds{0U};
    std::uint64_t respondedMicroseconds{0U};

    // Lower is better. Host pairs beat reflexive pairs beat relayed ones,
    // because a path that never leaves the building cannot be beaten on delay.
    [[nodiscard]] std::uint32_t priority() const noexcept;
    // Round trip, once the pair has answered. Zero until then.
    [[nodiscard]] std::uint64_t roundTripMicroseconds() const noexcept;
};

struct IceSettings final {
    // How often a given pair is retried. Fast enough that punching lands
    // inside a NAT's mapping window, slow enough not to look like a flood.
    std::uint64_t probeIntervalMicroseconds{200'000U};
    // Give up on a pair after this many unanswered probes.
    std::uint32_t probesBeforeFailure{7U};
    // Total time before the whole attempt is abandoned, so a hopeless network
    // reports failure rather than retrying forever.
    std::uint64_t overallTimeoutMicroseconds{20'000'000U};
    std::size_t maximumPairs{32U};
};

// What the caller should do next. The agent never sends anything itself.
struct IceAction final {
    bool sendProbe{false};
    IceCandidate to;
    IceCandidate from;
};

class IceAgent final {
public:
    explicit IceAgent(const IceSettings& settings = {});

    void addLocalCandidate(const IceCandidate& candidate);
    void addRemoteCandidate(const IceCandidate& candidate);

    // Builds the pair list. Called once both sides' candidates are known,
    // which is what the signalling exchange delivers.
    void beginChecks(std::uint64_t nowMicroseconds) noexcept;

    // The next probe due, if any. Returns nothing when every pair is either
    // waiting out its interval, already settled, or the attempt is over.
    [[nodiscard]] IceAction nextAction(std::uint64_t nowMicroseconds) noexcept;

    // A probe came back from this remote candidate. This is the only evidence
    // that a path works: a packet that left is not proof of anything, because
    // the far router may still be dropping it.
    void onProbeResponse(
        const IceCandidate& remote, std::uint64_t nowMicroseconds) noexcept;

    // The pair to carry audio on: the best-priority pair that actually
    // answered. Nothing until one has.
    [[nodiscard]] std::optional<CandidatePair> nominated() const noexcept;

    [[nodiscard]] bool connected() const noexcept { return nominated().has_value(); }
    // True once nothing further will succeed, which is when the musician should
    // be told rather than left watching a spinner.
    [[nodiscard]] bool exhausted(std::uint64_t nowMicroseconds) const noexcept;

    [[nodiscard]] const std::vector<CandidatePair>& pairs() const noexcept { return pairs_; }
    [[nodiscard]] std::size_t localCandidateCount() const noexcept {
        return local_.size();
    }

    void reset() noexcept;

private:
    IceSettings settings_;
    std::vector<IceCandidate> local_;
    std::vector<IceCandidate> remote_;
    std::vector<CandidatePair> pairs_;
    std::uint64_t startedMicroseconds_{0U};
    bool started_{false};
};

} // namespace jamlink::network
