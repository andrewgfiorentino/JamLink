// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace jamlink::clock {

struct ClockDomainControllerConfig final {
    std::size_t targetFillFrames{512};
    std::size_t capacityFrames{1'024};
    double proportionalPpmPerFrame{0.35};
    double integralPpmPerFrameSecond{0.1875};
    double maximumCorrectionPpm{250.0};
    double smoothingTimeSeconds{0.13};
};

// Produces an input-frames-per-output-frame correction ratio from ring
// occupancy. Elapsed frames and nominal rate make the controller independent of
// callback cadence. It is bounded and allocation-free; each instance belongs
// to one clock-domain consumer.
class ClockDomainController final {
public:
    explicit ClockDomainController(const ClockDomainControllerConfig& configuration);

    [[nodiscard]] double update(
        std::size_t currentFillFrames,
        std::size_t elapsedFrames,
        double nominalSampleRate) noexcept;
    [[nodiscard]] double correctionRatio() const noexcept { return correctionRatio_; }
    [[nodiscard]] double correctionPpm() const noexcept;
    void reset() noexcept;

private:
    ClockDomainControllerConfig configuration_;
    double integralError_{0.0};
    double correctionRatio_{1.0};
};

// Estimates relative long-term clock rate from monotonically increasing frame
// counters. This is telemetry/control-plane math, not wall-clock measurement.
class ClockDriftEstimator final {
public:
    explicit ClockDriftEstimator(double smoothing = 0.05);

    void update(std::uint64_t referenceFrames, std::uint64_t domainFrames) noexcept;
    [[nodiscard]] bool hasEstimate() const noexcept { return hasEstimate_; }
    [[nodiscard]] double driftPpm() const noexcept { return driftPpm_; }
    void reset() noexcept;

private:
    double smoothing_;
    std::uint64_t previousReferenceFrames_{0};
    std::uint64_t previousDomainFrames_{0};
    double driftPpm_{0.0};
    bool initialized_{false};
    bool hasEstimate_{false};
};

} // namespace jamlink::clock
