// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace jamlink::testing {

// Deterministic network impairment model for transport tests.
//
// The channel is driven by an explicit virtual clock and a seeded generator, so
// every run of a given seed produces byte-identical arrival schedules. Nothing
// here touches sockets, threads, or wall-clock time.
struct ImpairmentProfile final {
    std::uint64_t baseLatencyMicroseconds{15'000U};
    // Uniform +/- jitter applied per packet on top of the base latency.
    std::uint64_t jitterMicroseconds{0U};
    // Independent per-packet loss, expressed in parts per thousand.
    std::uint32_t lossPerThousand{0U};
    // Probability in parts per thousand that a burst starts on a given packet.
    std::uint32_t burstStartPerThousand{0U};
    std::uint32_t burstLengthPackets{0U};
    // Probability in parts per thousand that a packet is delivered twice.
    std::uint32_t duplicatePerThousand{0U};
    // Extra delay in packet periods applied to a randomly chosen packet,
    // producing genuine out-of-order arrival rather than uniform delay.
    std::uint32_t reorderPerThousand{0U};
    std::uint32_t reorderDepthPackets{2U};
};

class DeterministicChannel final {
public:
    struct Arrival final {
        std::uint32_t sequence{0U};
        std::uint64_t arrivalMicroseconds{0U};
    };

    DeterministicChannel(ImpairmentProfile profile, std::uint64_t seed)
        : profile_(profile), state_(seed == 0U ? 0x9E3779B97F4A7C15ULL : seed) {}

    // Offers one transmitted packet to the channel. Any resulting arrivals are
    // appended to pending(); callers drain them in timestamp order.
    void offer(
        std::uint32_t sequence,
        std::uint64_t sendMicroseconds,
        std::uint64_t packetPeriodMicroseconds) {
        if (burstRemaining_ > 0U) {
            --burstRemaining_;
            ++dropped_;
            return;
        }
        if (profile_.burstStartPerThousand != 0U
            && nextBelow(1'000U) < profile_.burstStartPerThousand) {
            burstRemaining_ = profile_.burstLengthPackets;
            if (burstRemaining_ > 0U) {
                --burstRemaining_;
                ++dropped_;
                return;
            }
        }
        if (profile_.lossPerThousand != 0U
            && nextBelow(1'000U) < profile_.lossPerThousand) {
            ++dropped_;
            return;
        }

        std::uint64_t delay = profile_.baseLatencyMicroseconds;
        if (profile_.jitterMicroseconds != 0U) {
            const std::uint64_t span = profile_.jitterMicroseconds * 2U + 1U;
            delay += nextBelow(span);
            delay = delay > profile_.jitterMicroseconds
                ? delay - profile_.jitterMicroseconds
                : 0U;
        }
        if (profile_.reorderPerThousand != 0U
            && nextBelow(1'000U) < profile_.reorderPerThousand) {
            delay += packetPeriodMicroseconds
                * (1U + nextBelow(std::max<std::uint32_t>(profile_.reorderDepthPackets, 1U)));
            ++reordered_;
        }

        pending_.push_back({sequence, sendMicroseconds + delay});
        ++delivered_;

        if (profile_.duplicatePerThousand != 0U
            && nextBelow(1'000U) < profile_.duplicatePerThousand) {
            pending_.push_back(
                {sequence, sendMicroseconds + delay + nextBelow(packetPeriodMicroseconds + 1U)});
            ++duplicated_;
        }
    }

    // Removes and returns every arrival at or before the supplied time, sorted
    // by arrival timestamp so the receiver observes true wire order.
    [[nodiscard]] std::vector<Arrival> drainUntil(std::uint64_t nowMicroseconds) {
        std::stable_sort(
            pending_.begin(), pending_.end(),
            [](const Arrival& left, const Arrival& right) {
                return left.arrivalMicroseconds < right.arrivalMicroseconds;
            });
        std::vector<Arrival> ready;
        auto split = pending_.begin();
        while (split != pending_.end() && split->arrivalMicroseconds <= nowMicroseconds) {
            ready.push_back(*split);
            ++split;
        }
        pending_.erase(pending_.begin(), split);
        return ready;
    }

    [[nodiscard]] std::size_t droppedPackets() const noexcept { return dropped_; }
    [[nodiscard]] std::size_t deliveredPackets() const noexcept { return delivered_; }
    [[nodiscard]] std::size_t duplicatedPackets() const noexcept { return duplicated_; }
    [[nodiscard]] std::size_t reorderedPackets() const noexcept { return reordered_; }

private:
    // splitmix64: small, dependency-free, and reproducible across platforms.
    [[nodiscard]] std::uint64_t next() noexcept {
        state_ += 0x9E3779B97F4A7C15ULL;
        std::uint64_t value = state_;
        value = (value ^ (value >> 30U)) * 0xBF58476D1CE4E5B9ULL;
        value = (value ^ (value >> 27U)) * 0x94D049BB133111EBULL;
        return value ^ (value >> 31U);
    }

    [[nodiscard]] std::uint64_t nextBelow(std::uint64_t bound) noexcept {
        return bound == 0U ? 0U : next() % bound;
    }

    ImpairmentProfile profile_;
    std::uint64_t state_;
    std::vector<Arrival> pending_;
    std::uint32_t burstRemaining_{0U};
    std::size_t dropped_{0U};
    std::size_t delivered_{0U};
    std::size_t duplicated_{0U};
    std::size_t reordered_{0U};
};

} // namespace jamlink::testing
