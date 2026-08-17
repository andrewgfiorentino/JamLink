// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/clock/clock_domain_controller.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace jamlink::clock {

ClockDomainController::ClockDomainController(const ClockDomainControllerConfig& configuration)
    : configuration_(configuration) {
    if (configuration_.capacityFrames == 0U
        || configuration_.targetFillFrames >= configuration_.capacityFrames) {
        throw std::invalid_argument("Clock-domain fill target must be inside the ring capacity");
    }
    if (!std::isfinite(configuration_.proportionalPpmPerFrame)
        || !std::isfinite(configuration_.integralPpmPerFrameSecond)
        || !std::isfinite(configuration_.maximumCorrectionPpm)
        || !std::isfinite(configuration_.smoothingTimeSeconds)
        || configuration_.proportionalPpmPerFrame < 0.0
        || configuration_.integralPpmPerFrameSecond < 0.0
        || configuration_.maximumCorrectionPpm <= 0.0
        || configuration_.maximumCorrectionPpm >= 1.0e6
        || configuration_.smoothingTimeSeconds <= 0.0) {
        throw std::invalid_argument("Invalid clock-domain controller tuning");
    }
}

double ClockDomainController::update(
    std::size_t currentFillFrames,
    std::size_t elapsedFrames,
    double nominalSampleRate) noexcept {
    if (elapsedFrames == 0U || !std::isfinite(nominalSampleRate)
        || nominalSampleRate <= 0.0) {
        return correctionRatio_;
    }

    const double elapsedSeconds = static_cast<double>(elapsedFrames) / nominalSampleRate;
    const std::size_t boundedFill = std::min(currentFillFrames, configuration_.capacityFrames);
    const double errorFrames = static_cast<double>(boundedFill)
        - static_cast<double>(configuration_.targetFillFrames);

    integralError_ += errorFrames * elapsedSeconds;
    const double integralLimit = configuration_.maximumCorrectionPpm
        / std::max(configuration_.integralPpmPerFrameSecond, 1.0e-12);
    integralError_ = std::clamp(integralError_, -integralLimit, integralLimit);

    const double requestedPpm = std::clamp(
        errorFrames * configuration_.proportionalPpmPerFrame
            + integralError_ * configuration_.integralPpmPerFrameSecond,
        -configuration_.maximumCorrectionPpm,
        configuration_.maximumCorrectionPpm);
    const double requestedRatio = 1.0 + requestedPpm * 1.0e-6;
    const double smoothing = elapsedSeconds
        / (configuration_.smoothingTimeSeconds + elapsedSeconds);
    correctionRatio_ += smoothing * (requestedRatio - correctionRatio_);
    return correctionRatio_;
}

double ClockDomainController::correctionPpm() const noexcept {
    return (correctionRatio_ - 1.0) * 1.0e6;
}

void ClockDomainController::reset() noexcept {
    integralError_ = 0.0;
    correctionRatio_ = 1.0;
}

ClockDriftEstimator::ClockDriftEstimator(double smoothing)
    : smoothing_(smoothing) {
    if (!std::isfinite(smoothing_) || smoothing_ <= 0.0 || smoothing_ > 1.0) {
        throw std::invalid_argument("Clock drift smoothing must be in (0, 1]");
    }
}

void ClockDriftEstimator::update(
    std::uint64_t referenceFrames,
    std::uint64_t domainFrames) noexcept {
    if (!initialized_) {
        previousReferenceFrames_ = referenceFrames;
        previousDomainFrames_ = domainFrames;
        initialized_ = true;
        return;
    }

    if (referenceFrames <= previousReferenceFrames_ || domainFrames <= previousDomainFrames_) {
        return;
    }

    const auto referenceDelta = referenceFrames - previousReferenceFrames_;
    const auto domainDelta = domainFrames - previousDomainFrames_;
    const double rawPpm =
        (static_cast<double>(domainDelta) / static_cast<double>(referenceDelta) - 1.0) * 1.0e6;

    driftPpm_ = hasEstimate_
        ? driftPpm_ + smoothing_ * (rawPpm - driftPpm_)
        : rawPpm;
    hasEstimate_ = true;
    previousReferenceFrames_ = referenceFrames;
    previousDomainFrames_ = domainFrames;
}

void ClockDriftEstimator::reset() noexcept {
    previousReferenceFrames_ = 0U;
    previousDomainFrames_ = 0U;
    driftPpm_ = 0.0;
    initialized_ = false;
    hasEstimate_ = false;
}

} // namespace jamlink::clock
