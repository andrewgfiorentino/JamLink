// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/ice_agent.hpp"

#include <algorithm>

namespace jamlink::network {
namespace {

[[nodiscard]] std::uint32_t kindWeight(CandidateKind kind) noexcept {
    switch (kind) {
    case CandidateKind::Host: return 0U;
    case CandidateKind::ServerReflexive: return 100U;
    case CandidateKind::Relayed: return 200U;
    }
    return 300U;
}

} // namespace

std::uint32_t CandidatePair::priority() const noexcept {
    // A pair is only as good as its worse half: a host address paired with a
    // relayed one still goes through the relay.
    return std::max(kindWeight(local.kind), kindWeight(remote.kind))
        + kindWeight(local.kind) + kindWeight(remote.kind);
}

std::uint64_t CandidatePair::roundTripMicroseconds() const noexcept {
    if (state != PairState::Succeeded || respondedMicroseconds <= firstProbeMicroseconds) {
        return 0U;
    }
    return respondedMicroseconds - firstProbeMicroseconds;
}

IceAgent::IceAgent(const IceSettings& settings) : settings_(settings) {}

void IceAgent::addLocalCandidate(const IceCandidate& candidate) {
    if (!candidate.valid()
        || std::find(local_.begin(), local_.end(), candidate) != local_.end()) {
        return;
    }
    local_.push_back(candidate);
}

void IceAgent::addRemoteCandidate(const IceCandidate& candidate) {
    if (!candidate.valid()
        || std::find(remote_.begin(), remote_.end(), candidate) != remote_.end()) {
        return;
    }
    remote_.push_back(candidate);
}

void IceAgent::beginChecks(std::uint64_t nowMicroseconds) noexcept {
    pairs_.clear();
    // Every local candidate against every remote one. Which combination works
    // cannot be predicted: it depends on two routers whose behaviour is not
    // observable from either end.
    for (const auto& localCandidate : local_) {
        for (const auto& remoteCandidate : remote_) {
            if (pairs_.size() >= settings_.maximumPairs) {
                break;
            }
            CandidatePair pair;
            pair.local = localCandidate;
            pair.remote = remoteCandidate;
            pairs_.push_back(pair);
        }
    }
    // Best first, so the earliest probe is also the one most worth winning.
    std::stable_sort(pairs_.begin(), pairs_.end(),
        [](const CandidatePair& left, const CandidatePair& right) {
            return left.priority() < right.priority();
        });
    startedMicroseconds_ = nowMicroseconds;
    started_ = true;
}

IceAction IceAgent::nextAction(std::uint64_t nowMicroseconds) noexcept {
    IceAction action;
    if (!started_ || connected() || exhausted(nowMicroseconds)) {
        return action;
    }
    for (auto& pair : pairs_) {
        if (pair.state == PairState::Succeeded || pair.state == PairState::Failed) {
            continue;
        }
        if (pair.probesSent >= settings_.probesBeforeFailure) {
            pair.state = PairState::Failed;
            continue;
        }
        if (pair.probesSent != 0U) {
            const std::uint64_t due =
                pair.firstProbeMicroseconds
                + static_cast<std::uint64_t>(pair.probesSent)
                    * settings_.probeIntervalMicroseconds;
            if (nowMicroseconds < due) {
                continue;
            }
        }
        if (pair.probesSent == 0U) {
            pair.firstProbeMicroseconds = nowMicroseconds;
        }
        ++pair.probesSent;
        pair.state = PairState::Probing;
        action.sendProbe = true;
        action.to = pair.remote;
        action.from = pair.local;
        return action;
    }
    return action;
}

void IceAgent::onProbeResponse(
    const IceCandidate& remote, std::uint64_t nowMicroseconds) noexcept {
    for (auto& pair : pairs_) {
        if (!(pair.remote == remote) || pair.state == PairState::Failed) {
            continue;
        }
        // A response is the only proof a path carries traffic in both
        // directions. Sending a probe proves nothing on its own, because the
        // far router may be discarding every one of them.
        if (pair.state != PairState::Succeeded) {
            pair.state = PairState::Succeeded;
            pair.respondedMicroseconds = nowMicroseconds;
        }
    }
}

std::optional<CandidatePair> IceAgent::nominated() const noexcept {
    const CandidatePair* best = nullptr;
    for (const auto& pair : pairs_) {
        if (pair.state != PairState::Succeeded) {
            continue;
        }
        // Pairs are already in priority order, so the first success is the best
        // available path rather than merely the first to answer. A LAN pair
        // that replies slightly later still beats a reflexive one.
        if (best == nullptr || pair.priority() < best->priority()) {
            best = &pair;
        }
    }
    if (best == nullptr) {
        return std::nullopt;
    }
    return *best;
}

bool IceAgent::exhausted(std::uint64_t nowMicroseconds) const noexcept {
    if (!started_ || connected()) {
        return false;
    }
    if (nowMicroseconds - startedMicroseconds_ >= settings_.overallTimeoutMicroseconds) {
        return true;
    }
    if (pairs_.empty()) {
        return true;
    }
    return std::all_of(pairs_.begin(), pairs_.end(), [this](const CandidatePair& pair) {
        return pair.state == PairState::Failed
            || pair.probesSent >= settings_.probesBeforeFailure;
    });
}

void IceAgent::reset() noexcept {
    local_.clear();
    remote_.clear();
    pairs_.clear();
    startedMicroseconds_ = 0U;
    started_ = false;
}

} // namespace jamlink::network
