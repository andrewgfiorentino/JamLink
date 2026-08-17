// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// What the codec owes a musician.
//
// Compression is worth having only if it costs neither delay nor the ability to
// recover from loss. These tests measure both rather than trusting the
// configuration: the bitrate that actually leaves the encoder, the delay
// actually introduced, what happens to the frames after a gap, and whether
// anything allocates once a session is running.

#include "jamlink/audio/opus_stream_codec.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <new>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

std::atomic<bool> allocationTrackingEnabled{false};
std::atomic<std::size_t> trackedAllocationCount{0};

} // namespace

void* operator new(std::size_t size) {
    if (allocationTrackingEnabled.load(std::memory_order_relaxed)) {
        trackedAllocationCount.fetch_add(1U, std::memory_order_relaxed);
    }
    if (void* memory = std::malloc(size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

namespace {

using TestFunction = std::function<void()>;

struct TestCase final {
    std::string name;
    TestFunction function;
};

std::vector<TestCase>& tests() {
    static std::vector<TestCase> allTests;
    return allTests;
}

struct RegisterTest final {
    RegisterTest(std::string name, TestFunction function) {
        tests().push_back(TestCase{std::move(name), std::move(function)});
    }
};

[[noreturn]] void fail(const char* expression, const char* file, int line) {
    throw std::runtime_error(
        std::string(file) + ':' + std::to_string(line) + " expectation failed: " + expression);
}

#define JAMLINK_TEST(name) \
    void name(); \
    const RegisterTest register_##name(#name, name); \
    void name()

#define EXPECT_TRUE(expression) \
    do { if (!(expression)) { fail(#expression, __FILE__, __LINE__); } } while (false)

using jamlink::audio::OpusStreamDecoder;
using jamlink::audio::OpusStreamEncoder;

constexpr std::uint32_t sampleRate = 48'000U;
// The packet the transport already sends: 5 ms at 48 kHz.
constexpr std::size_t frameSamples = 240U;
constexpr std::uint32_t defaultBitrate = 96'000U;

// A plucked string: a non-integer fundamental with several harmonics and a
// decaying envelope, which stresses a codec far more than a single sine.
[[nodiscard]] std::vector<float> makeInstrument(std::size_t frames) {
    std::vector<float> signal(frames, 0.0F);
    for (std::size_t index = 0U; index < frames; ++index) {
        const double time = static_cast<double>(index) / static_cast<double>(sampleRate);
        const double phase = 2.0 * std::numbers::pi * 146.83 * time;
        const double envelope = 0.35 + 0.65 * std::exp(-std::fmod(time, 1.6) * 1.4);
        const double value = 0.50 * std::sin(phase)
            + 0.26 * std::sin(2.0 * phase)
            + 0.14 * std::sin(3.0 * phase)
            + 0.07 * std::sin(5.0 * phase)
            + 0.03 * std::sin(7.0 * phase);
        signal[index] = static_cast<float>(value * envelope * 0.6);
    }
    return signal;
}

struct RoundTrip final {
    std::vector<float> decoded;
    std::size_t totalPacketBytes{0U};
    std::size_t packets{0U};
    std::size_t largestPacket{0U};
};

// Encodes and decodes the whole signal, optionally losing the packets whose
// index appears in `lost`.
[[nodiscard]] RoundTrip roundTrip(
    std::span<const float> source,
    std::uint32_t bitsPerSecond,
    const std::vector<std::size_t>& lost = {}) {
    OpusStreamEncoder encoder(
        sampleRate, frameSamples, bitsPerSecond, OpusStreamEncoder::Content::Music);
    OpusStreamDecoder decoder(sampleRate, frameSamples);
    if (!encoder.valid() || !decoder.valid()) {
        throw std::runtime_error("codec did not initialise");
    }

    RoundTrip result;
    result.decoded.reserve(source.size());
    std::vector<std::uint8_t> packet(OpusStreamEncoder::maximumPacketBytes(), 0U);
    std::vector<float> frame(frameSamples, 0.0F);

    const std::size_t frameCount = source.size() / frameSamples;
    for (std::size_t index = 0U; index < frameCount; ++index) {
        const auto input = std::span<const float>(source.data() + index * frameSamples, frameSamples);
        const std::size_t written = encoder.encode(input, std::span<std::uint8_t>(packet));
        if (written == 0U) {
            throw std::runtime_error("encoder produced nothing");
        }
        result.totalPacketBytes += written;
        result.largestPacket = std::max(result.largestPacket, written);
        ++result.packets;

        const bool dropped = std::find(lost.begin(), lost.end(), index) != lost.end();
        if (dropped) {
            static_cast<void>(decoder.conceal(std::span<float>(frame)));
        } else {
            static_cast<void>(decoder.decode(
                std::span<const std::uint8_t>(packet.data(), written), std::span<float>(frame)));
        }
        result.decoded.insert(result.decoded.end(), frame.begin(), frame.end());
    }
    return result;
}

[[nodiscard]] double energy(std::span<const float> block) {
    double total = 0.0;
    for (const float sample : block) {
        total += static_cast<double>(sample) * static_cast<double>(sample);
    }
    return total;
}

// Correlation at a given lag, which is how the codec's delay is found and then
// removed before quality is judged.
[[nodiscard]] double correlationAtLag(
    std::span<const float> source,
    std::span<const float> decoded,
    std::size_t lag,
    std::size_t from,
    std::size_t count) {
    double dot = 0.0;
    double sourceEnergy = 0.0;
    double decodedEnergy = 0.0;
    for (std::size_t index = 0U; index < count; ++index) {
        if (from + index >= source.size() || from + index + lag >= decoded.size()) {
            break;
        }
        const double a = static_cast<double>(source[from + index]);
        const double b = static_cast<double>(decoded[from + index + lag]);
        dot += a * b;
        sourceEnergy += a * a;
        decodedEnergy += b * b;
    }
    if (sourceEnergy <= 0.0 || decodedEnergy <= 0.0) {
        return 0.0;
    }
    return dot / std::sqrt(sourceEnergy * decodedEnergy);
}

struct Alignment final {
    std::size_t lag{0U};
    double correlation{0.0};
};

[[nodiscard]] Alignment bestAlignment(
    std::span<const float> source,
    std::span<const float> decoded,
    std::size_t maximumLag) {
    Alignment best;
    // Start past the beginning so the encoder's initial ramp is not judged.
    constexpr std::size_t from = frameSamples * 40U;
    constexpr std::size_t count = frameSamples * 200U;
    for (std::size_t lag = 0U; lag <= maximumLag; ++lag) {
        const double value = correlationAtLag(source, decoded, lag, from, count);
        if (value > best.correlation) {
            best.correlation = value;
            best.lag = lag;
        }
    }
    return best;
}

JAMLINK_TEST(the_codec_delivers_the_bitrate_it_was_asked_for) {
    // The whole point of compressing is a number a musician can check against
    // their uplink, so the number has to be true.
    const auto source = makeInstrument(frameSamples * 1'000U);
    const RoundTrip result = roundTrip(source, defaultBitrate);

    const double seconds =
        static_cast<double>(result.packets * frameSamples) / static_cast<double>(sampleRate);
    const double measured = static_cast<double>(result.totalPacketBytes) * 8.0 / seconds;
    // Constant bitrate, so this should be tight. The allowance covers the
    // per-frame rounding of a byte-aligned packet.
    std::cout << "       measured " << static_cast<std::uint32_t>(measured)
              << " bit/s, largest packet " << result.largestPacket << " bytes\n";
    EXPECT_TRUE(measured > static_cast<double>(defaultBitrate) * 0.9);
    EXPECT_TRUE(measured < static_cast<double>(defaultBitrate) * 1.1);

    // And it is a real reduction against what JamLink sends today: one stream
    // of uncompressed 16-bit PCM at 48 kHz is 768 kbit/s.
    EXPECT_TRUE(measured < 768'000.0 * 0.2);
    // No packet may approach the largest Opus can emit, or the datagram budget
    // would need rechecking.
    EXPECT_TRUE(result.largestPacket < 200U);
}

JAMLINK_TEST(a_lower_bitrate_really_does_send_less) {
    const auto source = makeInstrument(frameSamples * 600U);
    const RoundTrip rich = roundTrip(source, 128'000U);
    const RoundTrip lean = roundTrip(source, 48'000U);
    EXPECT_TRUE(lean.totalPacketBytes * 2U < rich.totalPacketBytes);
}

JAMLINK_TEST(the_codec_costs_no_more_delay_than_it_claims) {
    // Restricted low delay is chosen so the codec adds 2.5 ms, not the 6.5 ms
    // the default mode would. On an ASIO interface that difference is larger
    // than the entire monitoring path, so it is measured rather than assumed.
    const auto source = makeInstrument(frameSamples * 400U);
    const RoundTrip result = roundTrip(source, defaultBitrate);

    const Alignment alignment = bestAlignment(
        std::span<const float>(source), std::span<const float>(result.decoded), frameSamples * 2U);
    const double milliseconds =
        static_cast<double>(alignment.lag) * 1'000.0 / static_cast<double>(sampleRate);
    std::cout << "       delay " << milliseconds << " ms (" << alignment.lag
              << " samples), correlation " << alignment.correlation << "\n";
    // 2.5 ms is 120 samples at 48 kHz. Allow a little either side for the
    // correlation peak landing on a neighbouring sample.
    EXPECT_TRUE(milliseconds <= 3.0);
    // And the alignment has to be real, or the figure above means nothing.
    EXPECT_TRUE(alignment.correlation > 0.9);
}

JAMLINK_TEST(what_comes_out_is_recognisably_what_went_in) {
    const auto source = makeInstrument(frameSamples * 400U);
    const RoundTrip result = roundTrip(source, defaultBitrate);
    const Alignment alignment = bestAlignment(
        std::span<const float>(source), std::span<const float>(result.decoded), frameSamples * 2U);

    std::cout << "       correlation " << alignment.correlation << "\n";
    // Opus is perceptual, so sample-exact comparison is meaningless. Waveform
    // correlation after removing the codec's delay is the honest measure.
    EXPECT_TRUE(alignment.correlation > 0.95);

    // Level must survive too: a codec that quietly attenuates would pass a
    // correlation test while sounding wrong next to the local monitor.
    const double sourceEnergy = energy(
        std::span<const float>(source.data() + frameSamples * 40U, frameSamples * 200U));
    const double decodedEnergy = energy(
        std::span<const float>(result.decoded.data() + frameSamples * 40U, frameSamples * 200U));
    EXPECT_TRUE(decodedEnergy > sourceEnergy * 0.7);
    EXPECT_TRUE(decodedEnergy < sourceEnergy * 1.4);
}

JAMLINK_TEST(the_frames_after_a_gap_are_still_right) {
    // The reason concealment must go through the decoder rather than be written
    // as silence somewhere else. Opus predicts across frames, so a decoder that
    // is never told about a gap keeps predicting from state the encoder has
    // moved past, and everything after the gap decodes wrong too.
    const auto source = makeInstrument(frameSamples * 400U);
    const std::vector<std::size_t> lost{120U, 121U, 122U, 260U};
    const RoundTrip result = roundTrip(source, defaultBitrate, lost);

    const Alignment alignment = bestAlignment(
        std::span<const float>(source), std::span<const float>(result.decoded), frameSamples * 2U);
    EXPECT_TRUE(alignment.correlation > 0.9);

    // Judge a stretch that begins well after the last gap: if decoder state had
    // diverged, this is where it would show.
    const std::size_t from = 300U * frameSamples;
    const double recovered = correlationAtLag(
        std::span<const float>(source),
        std::span<const float>(result.decoded),
        alignment.lag,
        from,
        frameSamples * 60U);
    std::cout << "       correlation after the gaps " << recovered << "\n";
    EXPECT_TRUE(recovered > 0.95);

    // And the concealed frames themselves are audio, not silence.
    const double concealedEnergy = energy(
        std::span<const float>(result.decoded.data() + 120U * frameSamples, frameSamples));
    EXPECT_TRUE(concealedEnergy > 0.0);
}

JAMLINK_TEST(encoding_and_decoding_never_allocate_once_running) {
    // Both run on threads that must not block. Opus allocates its state when
    // created and not afterwards, and this holds that true.
    OpusStreamEncoder encoder(
        sampleRate, frameSamples, defaultBitrate, OpusStreamEncoder::Content::Music);
    OpusStreamDecoder decoder(sampleRate, frameSamples);
    EXPECT_TRUE(encoder.valid() && decoder.valid());

    const auto source = makeInstrument(frameSamples * 100U);
    std::vector<std::uint8_t> packet(OpusStreamEncoder::maximumPacketBytes(), 0U);
    std::vector<float> frame(frameSamples, 0.0F);
    // Warm both sides before counting, so first-use lazy work is not blamed on
    // the steady state.
    static_cast<void>(encoder.encode(
        std::span<const float>(source.data(), frameSamples), std::span<std::uint8_t>(packet)));
    static_cast<void>(decoder.decode(
        std::span<const std::uint8_t>(packet.data(), 1U), std::span<float>(frame)));

    trackedAllocationCount.store(0U, std::memory_order_relaxed);
    allocationTrackingEnabled.store(true, std::memory_order_relaxed);
    for (std::size_t index = 1U; index < 100U; ++index) {
        const std::size_t written = encoder.encode(
            std::span<const float>(source.data() + index * frameSamples, frameSamples),
            std::span<std::uint8_t>(packet));
        static_cast<void>(decoder.decode(
            std::span<const std::uint8_t>(packet.data(), written), std::span<float>(frame)));
        static_cast<void>(decoder.conceal(std::span<float>(frame)));
    }
    allocationTrackingEnabled.store(false, std::memory_order_relaxed);
    EXPECT_TRUE(trackedAllocationCount.load(std::memory_order_relaxed) == 0U);
}

JAMLINK_TEST(a_frame_size_opus_cannot_carry_is_refused_at_construction) {
    // Failing here rather than at the first encode is the difference between a
    // build that will not start and a session that is silent for no stated
    // reason.
    OpusStreamEncoder encoder(
        sampleRate, 200U, defaultBitrate, OpusStreamEncoder::Content::Music);
    EXPECT_TRUE(!encoder.valid());
    OpusStreamDecoder decoder(sampleRate, 200U);
    EXPECT_TRUE(!decoder.valid());

    // And the sizes the transport might reasonably use are all accepted.
    for (const std::size_t samples : {120U, 240U, 480U, 960U}) {
        OpusStreamEncoder supported(
            sampleRate, samples, defaultBitrate, OpusStreamEncoder::Content::Music);
        EXPECT_TRUE(supported.valid());
    }
}

JAMLINK_TEST(voice_and_music_are_tuned_separately_and_both_work) {
    const auto source = makeInstrument(frameSamples * 200U);
    for (const auto content :
         {OpusStreamEncoder::Content::Music, OpusStreamEncoder::Content::Voice}) {
        OpusStreamEncoder encoder(sampleRate, frameSamples, 64'000U, content);
        OpusStreamDecoder decoder(sampleRate, frameSamples);
        EXPECT_TRUE(encoder.valid() && decoder.valid());
        std::vector<std::uint8_t> packet(OpusStreamEncoder::maximumPacketBytes(), 0U);
        std::vector<float> frame(frameSamples, 0.0F);
        const std::size_t written = encoder.encode(
            std::span<const float>(source.data(), frameSamples), std::span<std::uint8_t>(packet));
        EXPECT_TRUE(written > 0U);
        EXPECT_TRUE(decoder.decode(
            std::span<const std::uint8_t>(packet.data(), written),
            std::span<float>(frame)) == frameSamples);
    }
}

} // namespace

int main() {
    std::size_t failures = 0U;
    for (const auto& test : tests()) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cout << "[FAIL] " << test.name << ": " << error.what() << '\n';
        }
    }
    std::cout << (tests().size() - failures) << '/' << tests().size()
              << " opus codec tests passed\n";
    return failures == 0U ? 0 : 1;
}
