// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// What a router does to this machine on the way out, and whether a musician
// can host. Decided from two observations rather than from a real network, so
// the symmetric case -- which needs a particular router in a particular house
// to reproduce -- is testable at all.

#include "jamlink/network/nat_behaviour.hpp"

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

using jamlink::network::NatMappingBehaviour;
using jamlink::network::ObservedMapping;
using jamlink::network::classifyNatBehaviour;

[[nodiscard]] ObservedMapping seen(const char* address, std::uint16_t port) {
    return ObservedMapping{true, address, port};
}

JAMLINK_TEST(the_same_endpoint_to_both_servers_can_host) {
    const auto result = classifyNatBehaviour(
        seen("203.0.113.7", 41234), seen("203.0.113.7", 41234));
    EXPECT_TRUE(result.behaviour == NatMappingBehaviour::EndpointIndependent);
    EXPECT_TRUE(result.canHostDirectly());
    // Advice on a connection that will work is noise.
    EXPECT_TRUE(result.advice().empty());
}

JAMLINK_TEST(a_rewritten_port_is_the_field_failure_and_cannot_host) {
    // The exact shape of a symmetric router: a public address discovered, an
    // invite that looks correct, and a port that was only ever valid for the
    // server that observed it.
    const auto result = classifyNatBehaviour(
        seen("203.0.113.7", 41234), seen("203.0.113.7", 51999));
    EXPECT_TRUE(result.behaviour == NatMappingBehaviour::AddressOrPortDependent);
    EXPECT_TRUE(result.portRewritten);
    EXPECT_TRUE(!result.canHostDirectly());

    const std::string_view advice = result.advice();
    EXPECT_TRUE(!advice.empty());
    // The musician can act on who makes the invite. They cannot act on the
    // words "symmetric NAT", and being told them helps nobody.
    EXPECT_TRUE(advice.find("symmetric") == std::string_view::npos);
    EXPECT_TRUE(advice.find("NAT") == std::string_view::npos);
    EXPECT_TRUE(advice.find("STUN") == std::string_view::npos);
    EXPECT_TRUE(advice.find("invite") != std::string_view::npos);
    // And it must say the session is no worse the other way round, or the
    // advice reads as a downgrade nobody will take.
    EXPECT_TRUE(advice.find("identical") != std::string_view::npos);
}

JAMLINK_TEST(a_pool_of_external_addresses_also_cannot_host) {
    const auto result = classifyNatBehaviour(
        seen("203.0.113.7", 41234), seen("203.0.113.9", 41234));
    EXPECT_TRUE(result.behaviour == NatMappingBehaviour::AddressOrPortDependent);
    // The port survived; the address did not. Still fatal to an invite, and
    // the log should not claim a rewritten port that did not happen.
    EXPECT_TRUE(!result.portRewritten);
    EXPECT_TRUE(!result.canHostDirectly());
}

JAMLINK_TEST(one_answer_is_never_enough_to_convict) {
    // Wrongly claiming symmetric sends a musician to ask their friend to host
    // when they never needed to, so a single observation must not decide it.
    for (const auto& pair : {
             std::pair<ObservedMapping, ObservedMapping>{
                 seen("203.0.113.7", 41234), ObservedMapping{}},
             std::pair<ObservedMapping, ObservedMapping>{
                 ObservedMapping{}, seen("203.0.113.7", 41234)}}) {
        const auto result = classifyNatBehaviour(pair.first, pair.second);
        EXPECT_TRUE(result.behaviour == NatMappingBehaviour::Inconclusive);
        // Unproven is not the same as proven fine, but it must never block a
        // musician who would have connected perfectly well.
        EXPECT_TRUE(result.canHostDirectly());
        EXPECT_TRUE(result.advice().empty());
    }
}

JAMLINK_TEST(no_answer_at_all_is_reported_as_not_probed) {
    const auto result = classifyNatBehaviour(ObservedMapping{}, ObservedMapping{});
    EXPECT_TRUE(result.behaviour == NatMappingBehaviour::NotProbed);
    EXPECT_TRUE(result.canHostDirectly());
    EXPECT_TRUE(result.advice().empty());
}

JAMLINK_TEST(every_behaviour_is_named_for_diagnostics) {
    for (std::uint8_t raw = 0U;
         raw <= static_cast<std::uint8_t>(NatMappingBehaviour::AddressOrPortDependent);
         ++raw) {
        const auto behaviour = static_cast<NatMappingBehaviour>(raw);
        EXPECT_TRUE(!jamlink::network::natBehaviourName(behaviour).empty());
        EXPECT_TRUE(jamlink::network::natBehaviourName(behaviour) != "Unknown");
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
              << " NAT behaviour tests passed\n";
    return failures == 0U ? 0 : 1;
}
