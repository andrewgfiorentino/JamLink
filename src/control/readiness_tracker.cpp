// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/control/readiness_tracker.hpp"

#include <algorithm>

namespace jamlink::control {

void ReadinessTracker::setConfiguration(
    SetupComponent component,
    std::uint64_t fingerprint) noexcept {
    auto& state = states_[index(component)];
    if (!state.configured || state.fingerprint != fingerprint) {
        state.fingerprint = fingerprint;
        state.configured = true;
        state.verified = false;
        state.verifiedFingerprint = 0U;
    }
}

void ReadinessTracker::invalidate(SetupComponent component) noexcept {
    auto& state = states_[index(component)];
    state.verified = false;
    state.verifiedFingerprint = 0U;
}

bool ReadinessTracker::markVerified(
    SetupComponent component,
    std::uint64_t fingerprint) noexcept {
    auto& state = states_[index(component)];
    if (!state.configured || state.fingerprint != fingerprint) {
        return false;
    }

    state.verified = true;
    state.verifiedFingerprint = fingerprint;
    return true;
}

bool ReadinessTracker::isVerified(SetupComponent component) const noexcept {
    const auto& state = states_[index(component)];
    return state.configured && state.verified
        && state.fingerprint == state.verifiedFingerprint;
}

bool ReadinessTracker::allVerified() const noexcept {
    return std::all_of(states_.begin(), states_.end(), [](const ComponentState& state) {
        return state.configured && state.verified
            && state.fingerprint == state.verifiedFingerprint;
    });
}

JoinSafetyDecision ReadinessTracker::joinSafetyDecision() const noexcept {
    return JoinSafetyDecision{
        !isVerified(SetupComponent::Instrument),
        !isVerified(SetupComponent::Voice),
        isVerified(SetupComponent::Output)};
}

std::size_t ReadinessTracker::index(SetupComponent component) noexcept {
    return static_cast<std::size_t>(component);
}

} // namespace jamlink::control
