// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

namespace jamlink::audio {

// Stores a float through an always-lock-free 32-bit atomic. This avoids relying
// on implementation-specific std::atomic<float> behavior in real-time code.
class RealtimeAtomicFloat final {
public:
    explicit RealtimeAtomicFloat(float value = 0.0F) noexcept
        : bits_(std::bit_cast<std::uint32_t>(value)) {}

    void store(float value, std::memory_order order = std::memory_order_release) noexcept {
        bits_.store(std::bit_cast<std::uint32_t>(value), order);
    }

    [[nodiscard]] float load(
        std::memory_order order = std::memory_order_acquire) const noexcept {
        return std::bit_cast<float>(bits_.load(order));
    }

private:
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    std::atomic<std::uint32_t> bits_;
};

} // namespace jamlink::audio
