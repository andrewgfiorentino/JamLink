// SPDX-License-Identifier: GPL-3.0-or-later

#include "jamlink/audio/level_meter.hpp"
#include "jamlink/audio/private_soundcheck_processor.hpp"
#include "jamlink/audio/spsc_audio_ring.hpp"
#include "jamlink/clock/clock_domain_controller.hpp"

#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <string_view>

namespace {

constexpr std::size_t framesPerBlock = 128U;
constexpr std::uint32_t sampleRate = 48'000U;

std::size_t parseBlockCount(int argumentCount, char** arguments) {
    std::size_t blocks = 20'000U;
    if (argumentCount < 2) {
        return blocks;
    }

    const std::string_view value(arguments[1]);
    const auto result = std::from_chars(value.data(), value.data() + value.size(), blocks);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || blocks == 0U) {
        return 0U;
    }
    return blocks;
}

} // namespace

int main(int argumentCount, char** arguments) {
    const std::size_t blockCount = parseBlockCount(argumentCount, arguments);
    if (blockCount == 0U) {
        std::cerr << "Usage: jamlink_core_stress [positive-block-count]\n";
        return 2;
    }

    jamlink::audio::PrivateSoundcheckProcessor soundcheck;
    jamlink::audio::LevelMeter instrumentMeter;
    jamlink::audio::SpscAudioRing outputRing(1'024U, 2U);
    jamlink::clock::ClockDomainController clockController({
        512U, 1'024U, 0.35, 0.1875, 250.0, 0.13});

    std::array<float, framesPerBlock> instrument{};
    std::array<float, framesPerBlock> voice{};
    std::array<float, framesPerBlock * 2U> monitor{};
    std::array<float, framesPerBlock * 2U> rendered{};
    double phase = 0.0;
    constexpr double twoPi = 6.28318530717958647692;
    constexpr double phaseStep = twoPi * 440.0 / static_cast<double>(sampleRate);

    const auto start = std::chrono::steady_clock::now();
    for (std::size_t block = 0; block < blockCount; ++block) {
        for (std::size_t frame = 0; frame < framesPerBlock; ++frame) {
            instrument[frame] = static_cast<float>(0.5 * std::sin(phase));
            voice[frame] = static_cast<float>(0.1 * std::sin(phase * 0.5));
            phase += phaseStep;
            if (phase >= twoPi) {
                phase -= twoPi;
            }
        }

        instrumentMeter.process(instrument);
        soundcheck.process({instrument, voice}, monitor);
        if (outputRing.write(monitor) != framesPerBlock) {
            std::cerr << "Unexpected output-ring overrun at block " << block << '\n';
            return 1;
        }
        if (outputRing.readAndZeroFill(rendered) != framesPerBlock) {
            std::cerr << "Unexpected output-ring underrun at block " << block << '\n';
            return 1;
        }
        static_cast<void>(clockController.update(512U, framesPerBlock, sampleRate));
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double simulatedSeconds = static_cast<double>(blockCount * framesPerBlock)
        / static_cast<double>(sampleRate);
    const auto meter = instrumentMeter.snapshot();

    std::cout << "blocks=" << blockCount << '\n'
              << "simulated_audio_seconds=" << simulatedSeconds << '\n'
              << "wall_seconds=" << seconds << '\n'
              << "realtime_factor=" << simulatedSeconds / seconds << '\n'
              << "peak_dbfs=" << meter.peakDbfs << '\n'
              << "underruns=" << outputRing.underrunCount() << '\n'
              << "overruns=" << outputRing.overrunCount() << '\n';
    return 0;
}
