// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace jamlink::audio {

enum class NativeSampleFormat : std::uint8_t {
    Int16LittleEndian,
    Int24LittleEndian,
    Int32LittleEndian,
    Float32LittleEndian,
    Float64LittleEndian,
    Int16BigEndian,
    Int24BigEndian,
    Int32BigEndian,
    Float32BigEndian,
    Float64BigEndian
};

[[nodiscard]] constexpr std::size_t bytesPerNativeSample(
    NativeSampleFormat format) noexcept {
    switch (format) {
    case NativeSampleFormat::Int16LittleEndian:
    case NativeSampleFormat::Int16BigEndian:
        return 2U;
    case NativeSampleFormat::Int24LittleEndian:
    case NativeSampleFormat::Int24BigEndian:
        return 3U;
    case NativeSampleFormat::Int32LittleEndian:
    case NativeSampleFormat::Float32LittleEndian:
    case NativeSampleFormat::Int32BigEndian:
    case NativeSampleFormat::Float32BigEndian:
        return 4U;
    case NativeSampleFormat::Float64LittleEndian:
    case NativeSampleFormat::Float64BigEndian:
        return 8U;
    }
    return 0U;
}

namespace detail {

[[nodiscard]] inline std::uint32_t readU32Little(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

[[nodiscard]] inline std::uint32_t readU32Big(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint32_t>(bytes[3])
        | (static_cast<std::uint32_t>(bytes[2]) << 8U)
        | (static_cast<std::uint32_t>(bytes[1]) << 16U)
        | (static_cast<std::uint32_t>(bytes[0]) << 24U);
}

inline void writeU32Little(std::uint8_t* bytes, std::uint32_t value) noexcept {
    bytes[0] = static_cast<std::uint8_t>(value);
    bytes[1] = static_cast<std::uint8_t>(value >> 8U);
    bytes[2] = static_cast<std::uint8_t>(value >> 16U);
    bytes[3] = static_cast<std::uint8_t>(value >> 24U);
}

inline void writeU32Big(std::uint8_t* bytes, std::uint32_t value) noexcept {
    bytes[3] = static_cast<std::uint8_t>(value);
    bytes[2] = static_cast<std::uint8_t>(value >> 8U);
    bytes[1] = static_cast<std::uint8_t>(value >> 16U);
    bytes[0] = static_cast<std::uint8_t>(value >> 24U);
}

[[nodiscard]] inline float finiteUnit(double sample) noexcept {
    return std::isfinite(sample)
        ? static_cast<float>(std::clamp(sample, -1.0, 1.0)) : 0.0F;
}

} // namespace detail

inline void nativeSamplesToFloat(
    NativeSampleFormat format,
    const void* source,
    std::span<float> destination) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(source);
    const std::size_t stride = bytesPerNativeSample(format);
    for (std::size_t frame = 0U; frame < destination.size(); ++frame) {
        const std::uint8_t* sample = bytes + frame * stride;
        double value = 0.0;
        switch (format) {
        case NativeSampleFormat::Int16LittleEndian: {
            const std::uint16_t raw = static_cast<std::uint16_t>(sample[0])
                | (static_cast<std::uint16_t>(sample[1]) << 8U);
            value = static_cast<std::int16_t>(raw) / 32'768.0;
            break;
        }
        case NativeSampleFormat::Int16BigEndian: {
            const std::uint16_t raw = static_cast<std::uint16_t>(sample[1])
                | (static_cast<std::uint16_t>(sample[0]) << 8U);
            value = static_cast<std::int16_t>(raw) / 32'768.0;
            break;
        }
        case NativeSampleFormat::Int24LittleEndian:
        case NativeSampleFormat::Int24BigEndian: {
            std::uint32_t raw = format == NativeSampleFormat::Int24LittleEndian
                ? static_cast<std::uint32_t>(sample[0])
                    | (static_cast<std::uint32_t>(sample[1]) << 8U)
                    | (static_cast<std::uint32_t>(sample[2]) << 16U)
                : static_cast<std::uint32_t>(sample[2])
                    | (static_cast<std::uint32_t>(sample[1]) << 8U)
                    | (static_cast<std::uint32_t>(sample[0]) << 16U);
            if ((raw & 0x00800000U) != 0U) {
                raw |= 0xFF000000U;
            }
            value = static_cast<std::int32_t>(raw) / 8'388'608.0;
            break;
        }
        case NativeSampleFormat::Int32LittleEndian:
            value = static_cast<std::int32_t>(detail::readU32Little(sample))
                / 2'147'483'648.0;
            break;
        case NativeSampleFormat::Int32BigEndian:
            value = static_cast<std::int32_t>(detail::readU32Big(sample))
                / 2'147'483'648.0;
            break;
        case NativeSampleFormat::Float32LittleEndian:
        case NativeSampleFormat::Float32BigEndian: {
            const std::uint32_t raw = format == NativeSampleFormat::Float32LittleEndian
                ? detail::readU32Little(sample) : detail::readU32Big(sample);
            value = std::bit_cast<float>(raw);
            break;
        }
        case NativeSampleFormat::Float64LittleEndian:
        case NativeSampleFormat::Float64BigEndian: {
            std::uint64_t raw = 0U;
            for (std::size_t byte = 0U; byte < 8U; ++byte) {
                const std::size_t sourceByte = format == NativeSampleFormat::Float64LittleEndian
                    ? byte : 7U - byte;
                raw |= static_cast<std::uint64_t>(sample[sourceByte]) << (byte * 8U);
            }
            value = std::bit_cast<double>(raw);
            break;
        }
        }
        destination[frame] = detail::finiteUnit(value);
    }
}

inline void floatToNativeSamples(
    NativeSampleFormat format,
    std::span<const float> source,
    void* destination) noexcept {
    auto* bytes = static_cast<std::uint8_t*>(destination);
    const std::size_t stride = bytesPerNativeSample(format);
    for (std::size_t frame = 0U; frame < source.size(); ++frame) {
        std::uint8_t* sample = bytes + frame * stride;
        const double bounded = std::isfinite(source[frame])
            ? std::clamp(static_cast<double>(source[frame]), -1.0, 1.0) : 0.0;
        switch (format) {
        case NativeSampleFormat::Int16LittleEndian:
        case NativeSampleFormat::Int16BigEndian: {
            const auto value = static_cast<std::int16_t>(std::lrint(
                bounded < 0.0 ? bounded * 32'768.0 : bounded * 32'767.0));
            const auto raw = static_cast<std::uint16_t>(value);
            if (format == NativeSampleFormat::Int16LittleEndian) {
                sample[0] = static_cast<std::uint8_t>(raw);
                sample[1] = static_cast<std::uint8_t>(raw >> 8U);
            } else {
                sample[1] = static_cast<std::uint8_t>(raw);
                sample[0] = static_cast<std::uint8_t>(raw >> 8U);
            }
            break;
        }
        case NativeSampleFormat::Int24LittleEndian:
        case NativeSampleFormat::Int24BigEndian: {
            const auto value = static_cast<std::int32_t>(std::llround(
                bounded < 0.0 ? bounded * 8'388'608.0 : bounded * 8'388'607.0));
            const auto raw = static_cast<std::uint32_t>(value);
            if (format == NativeSampleFormat::Int24LittleEndian) {
                sample[0] = static_cast<std::uint8_t>(raw);
                sample[1] = static_cast<std::uint8_t>(raw >> 8U);
                sample[2] = static_cast<std::uint8_t>(raw >> 16U);
            } else {
                sample[2] = static_cast<std::uint8_t>(raw);
                sample[1] = static_cast<std::uint8_t>(raw >> 8U);
                sample[0] = static_cast<std::uint8_t>(raw >> 16U);
            }
            break;
        }
        case NativeSampleFormat::Int32LittleEndian:
        case NativeSampleFormat::Int32BigEndian: {
            const auto value = static_cast<std::int64_t>(std::llround(
                bounded < 0.0 ? bounded * 2'147'483'648.0 : bounded * 2'147'483'647.0));
            const auto raw = static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
            if (format == NativeSampleFormat::Int32LittleEndian) {
                detail::writeU32Little(sample, raw);
            } else {
                detail::writeU32Big(sample, raw);
            }
            break;
        }
        case NativeSampleFormat::Float32LittleEndian:
        case NativeSampleFormat::Float32BigEndian: {
            const std::uint32_t raw = std::bit_cast<std::uint32_t>(static_cast<float>(bounded));
            if (format == NativeSampleFormat::Float32LittleEndian) {
                detail::writeU32Little(sample, raw);
            } else {
                detail::writeU32Big(sample, raw);
            }
            break;
        }
        case NativeSampleFormat::Float64LittleEndian:
        case NativeSampleFormat::Float64BigEndian: {
            const std::uint64_t raw = std::bit_cast<std::uint64_t>(bounded);
            for (std::size_t byte = 0U; byte < 8U; ++byte) {
                const std::size_t destinationByte = format == NativeSampleFormat::Float64LittleEndian
                    ? byte : 7U - byte;
                sample[destinationByte] = static_cast<std::uint8_t>(raw >> (byte * 8U));
            }
            break;
        }
        }
    }
}

} // namespace jamlink::audio
