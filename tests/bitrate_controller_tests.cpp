// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// Down fast, up slowly, and never oscillating. A session that audibly swings
// between two qualities every few seconds is worse than one that sits at the
// lower one, so the asymmetry here is the whole design.

#include "jamlink/network/bitrate_controller.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

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

using jamlink::network::BitrateController;

JAMLINK_TEST(a_clean_link_never_leaves_the_top_of_the_ladder) {
    BitrateController controller;
    for (int report = 0; report < 200; ++report) {
        EXPECT_TRUE(!controller.observe(0U));
    }
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder.front());
    EXPECT_TRUE(controller.reductions() == 0U);
    EXPECT_TRUE(!controller.exhausted());
}

JAMLINK_TEST(one_bad_report_is_a_blip_and_is_not_acted_on) {
    // Loss arrives in bursts. Reacting to the first report of every burst
    // would drop the rate on a link that was about to be fine again.
    BitrateController controller;
    EXPECT_TRUE(!controller.observe(30U));
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder.front());
}

JAMLINK_TEST(sustained_loss_steps_down_one_rung_at_a_time) {
    BitrateController controller;
    EXPECT_TRUE(!controller.observe(20U));
    EXPECT_TRUE(controller.observe(20U));
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder[1]);
    // Still losing at the new rate, so it keeps going -- one rung per report
    // rather than collapsing to the floor on a single bad stretch.
    EXPECT_TRUE(controller.observe(20U));
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder[2]);
    EXPECT_TRUE(controller.observe(20U));
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder.back());
}

JAMLINK_TEST(the_floor_is_a_floor_and_says_so) {
    BitrateController controller;
    for (int report = 0; report < 40; ++report) {
        static_cast<void>(controller.observe(40U));
    }
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder.back());
    // Nothing further this can do will help, and continuing to look like it is
    // still adapting would misdescribe the connection.
    EXPECT_TRUE(controller.exhausted());
    EXPECT_TRUE(controller.reductions() == BitrateController::ladder.size() - 1U);
}

JAMLINK_TEST(recovery_is_slow_and_returns_all_the_way_to_the_top) {
    BitrateController controller;
    static_cast<void>(controller.observe(20U));
    static_cast<void>(controller.observe(20U));
    EXPECT_TRUE(controller.step() == 1U);

    // A handful of clean reports is not yet a recovery.
    for (int report = 0; report < 6; ++report) {
        EXPECT_TRUE(!controller.observe(0U));
    }
    EXPECT_TRUE(controller.step() == 1U);

    // Six seconds of them is. And a link that stays clean climbs the whole way
    // back rather than stopping one rung short for the rest of the session.
    for (int report = 0; report < 200; ++report) {
        static_cast<void>(controller.observe(0U));
    }
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder.front());
    EXPECT_TRUE(!controller.exhausted());
}

JAMLINK_TEST(a_link_in_the_middle_band_holds_its_rate) {
    // Between clean and bad nothing advances, so a connection sitting at three
    // percent loss does not drift up and down across the gap.
    BitrateController controller;
    static_cast<void>(controller.observe(20U));
    static_cast<void>(controller.observe(20U));
    const std::uint32_t held = controller.bitsPerSecond();
    for (int report = 0; report < 100; ++report) {
        EXPECT_TRUE(!controller.observe(3U));
    }
    EXPECT_TRUE(controller.bitsPerSecond() == held);
}

JAMLINK_TEST(a_burst_after_a_recovery_starts_over_rather_than_accumulating) {
    // Clean reports must not bank credit against a later burst, or a link that
    // alternates between fine and terrible would never step down at all.
    BitrateController controller;
    for (int report = 0; report < 11; ++report) {
        static_cast<void>(controller.observe(0U));
    }
    static_cast<void>(controller.observe(30U));
    EXPECT_TRUE(controller.observe(30U));
    EXPECT_TRUE(controller.step() == 1U);
}

JAMLINK_TEST(reset_returns_it_to_the_top) {
    BitrateController controller;
    for (int report = 0; report < 20; ++report) {
        static_cast<void>(controller.observe(50U));
    }
    controller.reset();
    EXPECT_TRUE(controller.bitsPerSecond() == BitrateController::ladder.front());
    EXPECT_TRUE(controller.reductions() == 0U);
    EXPECT_TRUE(!controller.exhausted());
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
              << " bitrate controller tests passed\n";
    return failures == 0U ? 0 : 1;
}
