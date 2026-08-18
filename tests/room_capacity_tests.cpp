// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// The guard a mesh cannot ship without. Everyone sends to everyone, so one
// more musician costs every existing musician more upload -- and without this,
// the person who adds a fifth would watch the whole room get worse at once and
// conclude JamLink is unreliable.

#include "jamlink/control/room_capacity.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
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

using jamlink::control::RoomCapacityEvidence;
using jamlink::control::RoomCapacityVerdict;
using jamlink::control::evaluateRoomCapacity;
using jamlink::control::maximumMeshParticipants;

JAMLINK_TEST(upstream_is_counted_per_other_musician_not_per_room) {
    // The whole reason this file exists. A duo sends one copy; a quartet sends
    // three, from every machine at once.
    RoomCapacityEvidence evidence;
    evidence.participants = 2U;
    const auto duo = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(duo.currentUpstreamBitsPerSecond == 2U * 96'000U);
    EXPECT_TRUE(duo.upstreamWithOneMoreBitsPerSecond == 2U * 2U * 96'000U);

    evidence.participants = 4U;
    const auto quartet = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(quartet.currentUpstreamBitsPerSecond == 3U * 2U * 96'000U);
}

JAMLINK_TEST(an_unknown_uplink_never_blocks_anyone) {
    // Almost nobody knows their upload speed. Refusing a musician on the
    // strength of a figure JamLink never had would be worse than admitting
    // them and adapting, which the send rate already does.
    RoomCapacityEvidence evidence;
    evidence.declaredUplinkBitsPerSecond = 0U;
    const auto result = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(result.verdict == RoomCapacityVerdict::Unverified);
    EXPECT_TRUE(result.canAdmitAnother);
    // And it says nothing, because a warning that fires whenever JamLink is
    // merely unsure is one people learn to dismiss.
    EXPECT_TRUE(result.advice().empty());
}

JAMLINK_TEST(a_generous_uplink_is_reported_as_comfortable_and_says_nothing) {
    RoomCapacityEvidence evidence;
    evidence.participants = 2U;
    evidence.declaredUplinkBitsPerSecond = 20'000'000U;
    const auto result = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(result.verdict == RoomCapacityVerdict::Comfortable);
    EXPECT_TRUE(result.canAdmitAnother);
    EXPECT_TRUE(result.advice().empty());
}

JAMLINK_TEST(a_thin_uplink_refuses_the_next_musician_before_the_room_degrades) {
    // The failure this prevents: everybody gets worse at once, and nothing on
    // screen connects that to the person who just joined.
    RoomCapacityEvidence evidence;
    evidence.participants = 3U;
    // A common domestic upload. Three people already cost 384 kbit/s from this
    // machine; a fourth would cost 576, past the usable share of this link.
    evidence.declaredUplinkBitsPerSecond = 800'000U;
    const auto result = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(result.verdict == RoomCapacityVerdict::WouldExceedUplink);
    EXPECT_TRUE(!result.canAdmitAnother);

    const std::string_view advice = result.advice();
    EXPECT_TRUE(!advice.empty());
    // Musician language: what they can act on is who is in the room, not a
    // figure in bits per second.
    EXPECT_TRUE(advice.find("bandwidth") == std::string_view::npos);
    EXPECT_TRUE(advice.find("kbit") == std::string_view::npos);
    EXPECT_TRUE(advice.find("upstream") == std::string_view::npos);
    // And it has to say the cost falls on everyone, or refusing looks arbitrary.
    EXPECT_TRUE(advice.find("everybody") != std::string_view::npos
        || advice.find("everyone") != std::string_view::npos);
}

JAMLINK_TEST(measured_strain_outranks_any_arithmetic) {
    // Somebody typing a generous number into a settings box does not make
    // their connection faster. What it is actually delivering is the answer.
    RoomCapacityEvidence evidence;
    evidence.participants = 2U;
    evidence.declaredUplinkBitsPerSecond = 100'000'000U;
    evidence.sendRateAlreadyReduced = true;
    const auto result = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(result.verdict == RoomCapacityVerdict::AlreadyStrained);
    EXPECT_TRUE(!result.canAdmitAnother);
    EXPECT_TRUE(!result.advice().empty());
}

JAMLINK_TEST(the_mesh_has_a_limit_that_bandwidth_cannot_buy_past) {
    // Paths grow with the square of the group, and so do the ways for one of
    // them to be the problem. A fast connection does not fix that.
    RoomCapacityEvidence evidence;
    evidence.participants = maximumMeshParticipants;
    evidence.declaredUplinkBitsPerSecond = 1'000'000'000U;
    const auto result = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(result.verdict == RoomCapacityVerdict::BeyondMeshLimit);
    EXPECT_TRUE(!result.canAdmitAnother);
    EXPECT_TRUE(!result.advice().empty());
}

JAMLINK_TEST(a_lowered_send_rate_makes_more_room_rather_than_less) {
    // The bitrate controller and this have to agree. A link that stepped down
    // to fit is a link with room again, or the two would fight: one lowering
    // the rate to make space and the other still refusing on the old figure.
    RoomCapacityEvidence evidence;
    evidence.participants = 3U;
    evidence.declaredUplinkBitsPerSecond = 800'000U;
    EXPECT_TRUE(!evaluateRoomCapacity(evidence).canAdmitAnother);

    evidence.bitsPerSecondPerStream = 32'000U;
    const auto lowered = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(lowered.canAdmitAnother);
    EXPECT_TRUE(lowered.verdict == RoomCapacityVerdict::Comfortable);
}

JAMLINK_TEST(a_room_of_one_is_not_asked_to_send_anything) {
    RoomCapacityEvidence evidence;
    evidence.participants = 1U;
    const auto result = evaluateRoomCapacity(evidence);
    EXPECT_TRUE(result.currentUpstreamBitsPerSecond == 0U);
    EXPECT_TRUE(result.canAdmitAnother);
}

JAMLINK_TEST(every_verdict_is_named_for_diagnostics) {
    for (std::uint8_t raw = 0U;
         raw <= static_cast<std::uint8_t>(RoomCapacityVerdict::BeyondMeshLimit); ++raw) {
        const auto verdict = static_cast<RoomCapacityVerdict>(raw);
        EXPECT_TRUE(!jamlink::control::capacityVerdictName(verdict).empty());
        EXPECT_TRUE(jamlink::control::capacityVerdictName(verdict) != "Unknown");
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
              << " room capacity tests passed\n";
    return failures == 0U ? 0 : 1;
}
