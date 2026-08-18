// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace jamlink::network {

// How much of the uplink to ask for, decided from what the far end says it is
// actually receiving.
//
// JamLink sends at a fixed rate and has no way to ask for less. On a thin or
// contended uplink that is the wrong shape of failure: the connection does not
// degrade, it breaks up, and the musician is left turning the buffer up until
// playing together stops being possible.
//
// The far end is the only witness that matters. Loss measured here says what
// this machine failed to receive, which is the other direction entirely, and
// adapting the send rate from it would respond to the wrong link.
//
// Down fast, up slowly. A rate that is too high is audible immediately; a rate
// that is too low is merely less good, and creeping back up to it every few
// seconds after a real problem would make the session oscillate audibly between
// two qualities, which is worse than sitting at the lower one.
class BitrateController final {
public:
    // Opus in restricted low delay, mono, at 5 ms frames. The top of the ladder
    // is the rate JamLink has always sent at. The floor is where a guitar still
    // arrives recognisably; below it the codec spends its bits on artefacts.
    static constexpr std::array<std::uint32_t, 4U> ladder{
        96'000U, 64'000U, 48'000U, 32'000U};

    struct Settings final {
        // Percent of packets the far end had to conceal. Chosen so ordinary
        // wireless jitter, which the buffer already absorbs, does not provoke
        // a change: only real loss does.
        std::uint8_t lossPercentToStepDown{5U};
        std::uint8_t lossPercentConsideredClean{1U};
        // Consecutive observations before acting. One bad report is a blip.
        std::uint8_t badObservationsToStepDown{2U};
        // Twelve clean reports at the half-second ping cadence is six seconds,
        // which is long enough that a recovery is real rather than a gap
        // between bursts.
        std::uint8_t cleanObservationsToStepUp{12U};
    };

    BitrateController() noexcept = default;
    explicit BitrateController(const Settings& settings) noexcept : settings_(settings) {}

    // One report from the far end. Returns true when the rate changed, so the
    // caller can reconfigure the encoder only when it has to.
    [[nodiscard]] bool observe(std::uint8_t farEndLossPercent) noexcept;

    [[nodiscard]] std::uint32_t bitsPerSecond() const noexcept { return ladder[step_]; }
    [[nodiscard]] std::size_t step() const noexcept { return step_; }
    // At the floor and still losing: the link cannot carry this session, and
    // nothing further this class can do will change that. Worth reporting
    // rather than continuing to look like it is still adapting.
    [[nodiscard]] bool exhausted() const noexcept {
        return step_ + 1U == ladder.size() && badObservations_ >= settings_.badObservationsToStepDown;
    }
    [[nodiscard]] std::uint32_t reductions() const noexcept { return reductions_; }

    void reset() noexcept;

private:
    Settings settings_{};
    std::size_t step_{0U};
    std::uint8_t badObservations_{0U};
    std::uint8_t cleanObservations_{0U};
    std::uint32_t reductions_{0U};
};

} // namespace jamlink::network
