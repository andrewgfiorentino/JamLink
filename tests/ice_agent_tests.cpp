// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// Hole punching is notoriously only observable between two real houses, which
// is exactly why the decisions are pulled out here and driven through virtual
// time. Every rule below was learned from a connection that failed in field
// testing, and none of them can be checked by looking at a router.

#include "jamlink/network/ice_agent.hpp"

#include <cstddef>
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

using jamlink::network::CandidateKind;
using jamlink::network::IceAgent;
using jamlink::network::IceCandidate;
using jamlink::network::IceSettings;
using jamlink::network::PairState;

[[nodiscard]] IceCandidate host(std::string address, std::uint16_t port) {
    return IceCandidate{std::move(address), port, CandidateKind::Host};
}

[[nodiscard]] IceCandidate reflexive(std::string address, std::uint16_t port) {
    return IceCandidate{std::move(address), port, CandidateKind::ServerReflexive};
}

// Two musicians in different houses: each has a LAN address and a public one.
[[nodiscard]] IceAgent twoHomes(const IceSettings& settings = {}) {
    IceAgent agent(settings);
    agent.addLocalCandidate(host("192.168.1.20", 41'234U));
    agent.addLocalCandidate(reflexive("203.0.113.7", 41'234U));
    agent.addRemoteCandidate(host("192.168.4.31", 55'100U));
    agent.addRemoteCandidate(reflexive("198.51.100.9", 55'100U));
    return agent;
}

JAMLINK_TEST(every_plausible_pair_is_tried_because_none_can_be_predicted) {
    // Which combination works depends on two routers whose behaviour is not
    // observable from either end, so guessing one and hoping is what the
    // current single-address invite does.
    auto agent = twoHomes();
    agent.beginChecks(0U);
    EXPECT_TRUE(agent.pairs().size() == 4U);
}

JAMLINK_TEST(a_path_that_never_leaves_the_building_is_tried_first) {
    // Two musicians in the same house should not have their audio routed out
    // to the internet and back. Host-to-host cannot be beaten on delay.
    auto agent = twoHomes();
    agent.beginChecks(0U);
    const auto first = agent.pairs().front();
    EXPECT_TRUE(first.local.kind == CandidateKind::Host);
    EXPECT_TRUE(first.remote.kind == CandidateKind::Host);

    const auto action = agent.nextAction(0U);
    EXPECT_TRUE(action.sendProbe);
    EXPECT_TRUE(action.to.address == "192.168.4.31");
}

JAMLINK_TEST(sending_a_probe_is_never_treated_as_a_working_path) {
    // The failure this exists to prevent. A packet leaving proves nothing: the
    // far router may be discarding every one of them, which is precisely what
    // happens when only one side punches.
    auto agent = twoHomes();
    agent.beginChecks(0U);
    for (std::uint64_t now = 0U; now < 3'000'000U; now += 100'000U) {
        static_cast<void>(agent.nextAction(now));
    }
    EXPECT_TRUE(!agent.connected());
    EXPECT_TRUE(!agent.nominated().has_value());
}

JAMLINK_TEST(a_reply_is_what_proves_the_path_carries_traffic_both_ways) {
    auto agent = twoHomes();
    agent.beginChecks(0U);
    static_cast<void>(agent.nextAction(0U));
    // The public pair answers, which is the ordinary two-home outcome.
    agent.onProbeResponse(reflexive("198.51.100.9", 55'100U), 40'000U);

    EXPECT_TRUE(agent.connected());
    const auto chosen = agent.nominated();
    EXPECT_TRUE(chosen.has_value());
    EXPECT_TRUE(chosen->remote.kind == CandidateKind::ServerReflexive);
    EXPECT_TRUE(chosen->state == PairState::Succeeded);
}

JAMLINK_TEST(a_local_path_wins_even_when_it_answers_second) {
    // Both may work when two musicians are in the same building. The one that
    // stays on the LAN is worth several milliseconds and must not lose merely
    // for replying a moment later.
    auto agent = twoHomes();
    agent.beginChecks(0U);
    static_cast<void>(agent.nextAction(0U));
    agent.onProbeResponse(reflexive("198.51.100.9", 55'100U), 30'000U);
    agent.onProbeResponse(host("192.168.4.31", 55'100U), 90'000U);

    const auto chosen = agent.nominated();
    EXPECT_TRUE(chosen.has_value());
    EXPECT_TRUE(chosen->remote.kind == CandidateKind::Host);
}

JAMLINK_TEST(probes_are_paced_rather_than_flooded) {
    // Fast enough to land inside a NAT mapping window, slow enough not to look
    // like an attack to anything watching.
    IceSettings settings;
    settings.probeIntervalMicroseconds = 200'000U;
    auto agent = twoHomes(settings);
    agent.beginChecks(0U);

    EXPECT_TRUE(agent.nextAction(0U).sendProbe);
    // Each remaining pair gets its first probe immediately; a pair already
    // probed waits out its interval.
    std::size_t immediate = 1U;
    while (agent.nextAction(0U).sendProbe) {
        ++immediate;
        EXPECT_TRUE(immediate <= 8U);
    }
    EXPECT_TRUE(immediate == 4U);
    // Nothing more is due until the interval elapses.
    EXPECT_TRUE(!agent.nextAction(100'000U).sendProbe);
    EXPECT_TRUE(agent.nextAction(200'000U).sendProbe);
}

JAMLINK_TEST(a_hopeless_network_gives_up_rather_than_spinning_forever) {
    // A musician watching a spinner that will never resolve is worse served
    // than one told plainly that this network cannot do it.
    IceSettings settings;
    settings.probesBeforeFailure = 3U;
    settings.probeIntervalMicroseconds = 100'000U;
    auto agent = twoHomes(settings);
    agent.beginChecks(0U);

    for (std::uint64_t now = 0U; now < 2'000'000U; now += 50'000U) {
        static_cast<void>(agent.nextAction(now));
    }
    EXPECT_TRUE(agent.exhausted(2'000'000U));
    EXPECT_TRUE(!agent.connected());
    // And it stops asking, so nothing keeps sending into a dead path.
    EXPECT_TRUE(!agent.nextAction(2'000'000U).sendProbe);
}

JAMLINK_TEST(the_overall_deadline_ends_an_attempt_that_would_otherwise_creep) {
    IceSettings settings;
    settings.overallTimeoutMicroseconds = 1'000'000U;
    settings.probesBeforeFailure = 1'000U;
    auto agent = twoHomes(settings);
    agent.beginChecks(0U);
    EXPECT_TRUE(!agent.exhausted(500'000U));
    EXPECT_TRUE(agent.exhausted(1'000'000U));
}

JAMLINK_TEST(success_stops_the_probing) {
    // Once a path is known to work, continuing to punch every other pair wastes
    // the musician's uplink at exactly the moment audio starts needing it.
    auto agent = twoHomes();
    agent.beginChecks(0U);
    static_cast<void>(agent.nextAction(0U));
    agent.onProbeResponse(host("192.168.4.31", 55'100U), 20'000U);
    EXPECT_TRUE(!agent.nextAction(1'000'000U).sendProbe);
}

JAMLINK_TEST(a_round_trip_is_measured_rather_than_assumed) {
    auto agent = twoHomes();
    agent.beginChecks(0U);
    static_cast<void>(agent.nextAction(1'000U));
    agent.onProbeResponse(host("192.168.4.31", 55'100U), 15'000U);
    const auto chosen = agent.nominated();
    EXPECT_TRUE(chosen.has_value());
    EXPECT_TRUE(chosen->roundTripMicroseconds() == 14'000U);
}

JAMLINK_TEST(duplicate_and_malformed_candidates_are_refused) {
    IceAgent agent;
    agent.addLocalCandidate(host("192.168.1.20", 41'234U));
    agent.addLocalCandidate(host("192.168.1.20", 41'234U));
    agent.addLocalCandidate(host("", 41'234U));
    agent.addLocalCandidate(host("192.168.1.20", 0U));
    EXPECT_TRUE(agent.localCandidateCount() == 1U);
}

JAMLINK_TEST(no_remote_candidates_is_a_failure_rather_than_a_wait) {
    // The signalling exchange never delivered. Saying so beats probing nothing
    // for twenty seconds.
    IceAgent agent;
    agent.addLocalCandidate(host("192.168.1.20", 41'234U));
    agent.beginChecks(0U);
    EXPECT_TRUE(agent.pairs().empty());
    EXPECT_TRUE(agent.exhausted(0U));
    EXPECT_TRUE(!agent.nextAction(0U).sendProbe);
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
              << " ICE agent tests passed\n";
    return failures == 0U ? 0 : 1;
}
