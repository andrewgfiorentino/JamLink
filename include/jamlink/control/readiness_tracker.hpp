// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace jamlink::control {

enum class SetupComponent : std::uint8_t {
    Instrument,
    Voice,
    Output,
    Count
};

struct JoinSafetyDecision final {
    bool instrumentStartsMuted{true};
    bool voiceStartsMuted{true};
    bool outputVerified{false};
};

// Control-thread state for the private Soundcheck readiness contract. A change
// to a device/routing fingerprint invalidates only the affected component.
class ReadinessTracker final {
public:
    void setConfiguration(SetupComponent component, std::uint64_t fingerprint) noexcept;
    void invalidate(SetupComponent component) noexcept;
    [[nodiscard]] bool markVerified(
        SetupComponent component,
        std::uint64_t fingerprint) noexcept;
    [[nodiscard]] bool isVerified(SetupComponent component) const noexcept;
    [[nodiscard]] bool allVerified() const noexcept;
    [[nodiscard]] JoinSafetyDecision joinSafetyDecision() const noexcept;

private:
    struct ComponentState final {
        std::uint64_t fingerprint{0};
        std::uint64_t verifiedFingerprint{0};
        bool configured{false};
        bool verified{false};
    };

    [[nodiscard]] static std::size_t index(SetupComponent component) noexcept;
    std::array<ComponentState, static_cast<std::size_t>(SetupComponent::Count)> states_{};
};

} // namespace jamlink::control
