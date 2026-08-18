// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// Two musicians who joined the same host have never heard of each other, and
// in a mesh they have to be connected directly. These check the introduction,
// and the part each end plays -- the thing that, got wrong in either direction,
// produces a session that silently never forms.

#include "jamlink/network/room_roster.hpp"

#include <algorithm>
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
using jamlink::network::IceCandidate;
using jamlink::network::PairRole;
using jamlink::network::RoomRoster;
using jamlink::network::RosterMember;

[[nodiscard]] RosterMember memberOf(
    const char* id, const char* name, const char* address, std::uint16_t port = 41'234U) {
    RosterMember member;
    member.participantId = id;
    member.displayName = name;
    if (address != nullptr) {
        member.candidates.push_back(
            IceCandidate{address, port, CandidateKind::ServerReflexive});
    }
    return member;
}

JAMLINK_TEST(both_ends_of_a_pair_choose_opposite_parts) {
    // The whole point. Each side runs this over the same two names without
    // exchanging anything, and two hosts or two guests is a session that never
    // forms and reports nothing while not forming.
    EXPECT_TRUE(RoomRoster::roleToward("andrew", "mike") == PairRole::Host);
    EXPECT_TRUE(RoomRoster::roleToward("mike", "andrew") == PairRole::Guest);
    // And it holds for every pair in a room, not just one.
    const std::vector<std::string> room{"andrew", "mike", "sam", "zoe"};
    for (const auto& first : room) {
        for (const auto& second : room) {
            if (first == second) {
                continue;
            }
            const auto here = RoomRoster::roleToward(first, second);
            const auto there = RoomRoster::roleToward(second, first);
            EXPECT_TRUE((here == PairRole::Host) != (there == PairRole::Host));
            EXPECT_TRUE(here != PairRole::Undecidable);
        }
    }
}

JAMLINK_TEST(nobody_connects_to_themselves_or_to_a_copied_identity) {
    // A profile identifier is self-asserted until the handshake authenticates
    // it. Somebody copying another musician's identity must not be able to
    // make them start a second session with what looks like their own name.
    EXPECT_TRUE(RoomRoster::roleToward("andrew", "andrew") == PairRole::Self);
    EXPECT_TRUE(RoomRoster::roleToward("", "mike") == PairRole::Undecidable);
    EXPECT_TRUE(RoomRoster::roleToward("andrew", "") == PairRole::Undecidable);
}

JAMLINK_TEST(a_third_musician_is_introduced_to_the_second) {
    // Andrew hosts, Mike and Sam each join him. Mike and Sam have never heard
    // of each other, and in a mesh they have to be connected directly.
    RoomRoster roster;
    EXPECT_TRUE(roster.remember(memberOf("andrew", "Andrew", "203.0.113.1")));
    EXPECT_TRUE(roster.remember(memberOf("mike", "Mike", "203.0.113.2")));
    EXPECT_TRUE(roster.remember(memberOf("sam", "Sam", "203.0.113.3")));

    const auto forMike = roster.peersToConnect("mike", {"andrew"});
    EXPECT_TRUE(forMike.size() == 1U);
    EXPECT_TRUE(forMike[0].member.participantId == "sam");

    const auto forSam = roster.peersToConnect("sam", {"andrew"});
    EXPECT_TRUE(forSam.size() == 1U);
    EXPECT_TRUE(forSam[0].member.participantId == "mike");
    // And they agree about which of them reaches out.
    EXPECT_TRUE((forMike[0].role == PairRole::Host) != (forSam[0].role == PairRole::Host));
}

JAMLINK_TEST(somebody_already_connected_is_not_connected_to_twice) {
    // The roster arrives more than once -- on every change, and again when
    // somebody reconnects. Acting on it must be idempotent or a room would
    // accumulate duplicate sessions with the same person.
    RoomRoster roster;
    EXPECT_TRUE(roster.remember(memberOf("andrew", "Andrew", "203.0.113.1")));
    EXPECT_TRUE(roster.remember(memberOf("mike", "Mike", "203.0.113.2")));
    EXPECT_TRUE(roster.peersToConnect("andrew", {"mike"}).empty());
    EXPECT_TRUE(roster.peersToConnect("andrew", {}).size() == 1U);
}

JAMLINK_TEST(somebody_who_reconnects_replaces_their_old_addresses) {
    // Their router gave them a different port. Keeping the old entry alongside
    // the new one would leave everyone still probing somewhere nobody is.
    RoomRoster roster;
    EXPECT_TRUE(roster.remember(memberOf("mike", "Mike", "203.0.113.2", 41'234U)));
    EXPECT_TRUE(roster.remember(memberOf("mike", "Mike", "198.51.100.9", 55'100U)));
    EXPECT_TRUE(roster.size() == 1U);
    const auto pending = roster.peersToConnect("andrew", {});
    EXPECT_TRUE(pending.size() == 1U);
    EXPECT_TRUE(pending[0].member.candidates.size() == 1U);
    EXPECT_TRUE(pending[0].member.candidates[0].address == "198.51.100.9");
}

JAMLINK_TEST(a_member_with_nowhere_to_be_reached_is_skipped_not_retried) {
    // Announced but not yet reachable. Attempting them would retry forever
    // against nothing, and look identical to a peer who is refusing.
    RoomRoster roster;
    EXPECT_TRUE(roster.remember(memberOf("mike", "Mike", nullptr)));
    EXPECT_TRUE(roster.contains("mike"));
    EXPECT_TRUE(roster.peersToConnect("andrew", {}).empty());
}

JAMLINK_TEST(a_nameless_entry_is_refused_rather_than_stored) {
    // Without a name there is nothing to sort by, so no pair containing this
    // entry could ever agree which end to be.
    RoomRoster roster;
    EXPECT_TRUE(!roster.remember(memberOf("", "Nobody", "203.0.113.4")));
    EXPECT_TRUE(roster.size() == 0U);
}

JAMLINK_TEST(the_room_is_bounded_and_says_when_it_is_full) {
    RoomRoster roster;
    for (std::size_t index = 0U; index < RoomRoster::maximumMembers; ++index) {
        const std::string id = "musician-" + std::to_string(index);
        EXPECT_TRUE(roster.remember(memberOf(id.c_str(), "X", "203.0.113.9")));
    }
    EXPECT_TRUE(!roster.remember(memberOf("one-too-many", "X", "203.0.113.9")));
    EXPECT_TRUE(roster.size() == RoomRoster::maximumMembers);
    // A musician already here can still update their addresses when the room
    // is full, or a reconnect would be refused exactly when it matters.
    EXPECT_TRUE(roster.remember(memberOf("musician-0", "X", "198.51.100.1")));
}

JAMLINK_TEST(leaving_twice_is_not_an_error) {
    RoomRoster roster;
    EXPECT_TRUE(roster.remember(memberOf("mike", "Mike", "203.0.113.2")));
    EXPECT_TRUE(roster.forget("mike"));
    // A leave notice can arrive from the person leaving and again from whoever
    // noticed they had gone.
    EXPECT_TRUE(!roster.forget("mike"));
    EXPECT_TRUE(roster.size() == 0U);
}

JAMLINK_TEST(every_role_is_named_for_diagnostics) {
    for (const auto role : {PairRole::Host, PairRole::Guest, PairRole::Self,
                            PairRole::Undecidable}) {
        EXPECT_TRUE(!jamlink::network::pairRoleName(role).empty());
        EXPECT_TRUE(jamlink::network::pairRoleName(role) != "Unknown");
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
              << " room roster tests passed\n";
    return failures == 0U ? 0 : 1;
}
