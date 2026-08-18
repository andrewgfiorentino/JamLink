// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/network/bitrate_controller.hpp"

namespace jamlink::network {

void BitrateController::reset() noexcept {
    step_ = 0U;
    badObservations_ = 0U;
    cleanObservations_ = 0U;
    reductions_ = 0U;
}

bool BitrateController::observe(std::uint8_t farEndLossPercent) noexcept {
    if (farEndLossPercent >= settings_.lossPercentToStepDown) {
        cleanObservations_ = 0U;
        if (badObservations_ < 255U) {
            ++badObservations_;
        }
        if (badObservations_ >= settings_.badObservationsToStepDown
            && step_ + 1U < ladder.size()) {
            ++step_;
            ++reductions_;
            // The counter is deliberately not cleared. Staying at this rate and
            // still losing has to be able to step down again on the next
            // report, and at the floor it has to leave `exhausted` true rather
            // than reading as though the problem had gone away.
            return true;
        }
        return false;
    }

    if (farEndLossPercent <= settings_.lossPercentConsideredClean) {
        badObservations_ = 0U;
        if (cleanObservations_ < 255U) {
            ++cleanObservations_;
        }
        if (cleanObservations_ >= settings_.cleanObservationsToStepUp && step_ > 0U) {
            --step_;
            cleanObservations_ = 0U;
            return true;
        }
        return false;
    }

    // Between the two thresholds: some loss, but not enough to act on. Neither
    // counter advances, so a link sitting in that band holds its rate instead
    // of drifting up and down across the gap -- which is exactly the
    // oscillation a listener would hear as the session changing quality every
    // few seconds.
    return false;
}

} // namespace jamlink::network
