// Copyright (c) 2026 Andrew Fiorentino
// SPDX-License-Identifier: GPL-3.0-or-later

// Keys that belong to a pair rather than to the room. With one peer the
// difference is invisible; with a mesh it is the difference between a private
// session and one where any member can read any other pair's audio, and where
// a packet addressed to one musician authenticates as one addressed to another.

#include "jamlink/network/peer_key_schedule.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

using jamlink::network::KeyDirection;
using jamlink::network::derivePeerKey;
using jamlink::network::pairDiscriminator;
using jamlink::network::peerNoncePrefixBytes;

// A stand-in for the platform HMAC. Not cryptographic and not meant to be:
// these tests are about the key schedule's structure -- that pairs are
// separated, that directions are separated, that both ends agree -- and none
// of that depends on which pseudorandom function is underneath.
bool testHmac(
    std::span<const std::uint8_t> key,
    std::span<const std::uint8_t> message,
    std::span<std::uint8_t, 32U> digest) {
    std::uint64_t state = 0xcbf2'9ce4'8422'2325ULL;
    const auto absorb = [&state](std::span<const std::uint8_t> bytes) {
        for (const std::uint8_t value : bytes) {
            state ^= value;
            state *= 0x100'0000'01b3ULL;
        }
    };
    absorb(key);
    absorb(message);
    for (std::size_t index = 0U; index < digest.size(); ++index) {
        state ^= state >> 33U;
        state *= 0xff51'afd7'ed55'8ccdULL;
        digest[index] = static_cast<std::uint8_t>(state >> 56U);
    }
    return true;
}

struct HmacFixture final {
    HmacFixture() { jamlink::network::installHmacSha256(testHmac); }
    ~HmacFixture() { jamlink::network::installHmacSha256(nullptr); }
};

[[nodiscard]] std::array<std::uint8_t, 32U> secretOf(std::uint8_t seed) {
    std::array<std::uint8_t, 32U> secret{};
    secret.fill(seed);
    return secret;
}

[[nodiscard]] std::array<std::uint8_t, peerNoncePrefixBytes> prefixOf(std::uint8_t seed) {
    std::array<std::uint8_t, peerNoncePrefixBytes> prefix{};
    prefix.fill(seed);
    return prefix;
}

[[nodiscard]] std::array<std::uint8_t, 32U> keyFor(
    std::uint8_t secretSeed,
    std::uint8_t localSeed,
    std::uint8_t remoteSeed,
    KeyDirection direction) {
    const auto secret = secretOf(secretSeed);
    const auto local = prefixOf(localSeed);
    const auto remote = prefixOf(remoteSeed);
    std::array<std::uint8_t, 32U> key{};
    const bool derived = derivePeerKey(
        std::span<const std::uint8_t, 32U>(secret),
        std::span<const std::uint8_t, peerNoncePrefixBytes>(local),
        std::span<const std::uint8_t, peerNoncePrefixBytes>(remote),
        direction, std::span<std::uint8_t, 32U>(key));
    if (!derived) {
        throw std::runtime_error("derivation failed");
    }
    return key;
}

JAMLINK_TEST(both_ends_of_a_pair_derive_the_same_key) {
    // The whole schedule rests on this. Each side sees the two prefixes in the
    // opposite order, and if that produced different keys nothing would ever
    // decrypt -- which is the failure a naive concatenation would cause.
    const HmacFixture fixture;
    const auto fromOneSide = keyFor(7U, 1U, 2U, KeyDirection::HostToGuest);
    const auto fromTheOther = keyFor(7U, 2U, 1U, KeyDirection::HostToGuest);
    EXPECT_TRUE(fromOneSide == fromTheOther);
}

JAMLINK_TEST(two_pairs_in_one_room_do_not_share_a_key) {
    // The defect this exists to remove: with a room-wide key, a third musician
    // holds everything needed to read a conversation they are not part of.
    const HmacFixture fixture;
    const auto pairOne = keyFor(7U, 1U, 2U, KeyDirection::HostToGuest);
    const auto pairTwo = keyFor(7U, 1U, 3U, KeyDirection::HostToGuest);
    const auto pairThree = keyFor(7U, 2U, 3U, KeyDirection::HostToGuest);
    EXPECT_TRUE(pairOne != pairTwo);
    EXPECT_TRUE(pairOne != pairThree);
    EXPECT_TRUE(pairTwo != pairThree);
}

JAMLINK_TEST(the_two_directions_of_one_pair_do_not_share_a_key) {
    // Without this a packet could be reflected back at its sender and still
    // authenticate, which is an attack this project has already had to fix
    // once and must not reintroduce per pair.
    const HmacFixture fixture;
    const auto out = keyFor(7U, 1U, 2U, KeyDirection::HostToGuest);
    const auto back = keyFor(7U, 1U, 2U, KeyDirection::GuestToHost);
    EXPECT_TRUE(out != back);
}

JAMLINK_TEST(a_different_room_gives_a_different_key_for_the_same_pair) {
    const HmacFixture fixture;
    EXPECT_TRUE(keyFor(7U, 1U, 2U, KeyDirection::HostToGuest)
        != keyFor(8U, 1U, 2U, KeyDirection::HostToGuest));
}

JAMLINK_TEST(a_key_is_never_the_room_secret_itself) {
    // A derivation that passed the secret through would hand every musician
    // the room key in the clear the moment one of them was compromised.
    const HmacFixture fixture;
    const auto secret = secretOf(7U);
    const auto key = keyFor(7U, 1U, 2U, KeyDirection::HostToGuest);
    EXPECT_TRUE(std::memcmp(key.data(), secret.data(), key.size()) != 0);
}

JAMLINK_TEST(the_pair_discriminator_is_symmetric_and_distinguishing) {
    const auto one = prefixOf(1U);
    const auto two = prefixOf(2U);
    EXPECT_TRUE(pairDiscriminator(one, two) == pairDiscriminator(two, one));
    const auto three = prefixOf(3U);
    EXPECT_TRUE(pairDiscriminator(one, two) != pairDiscriminator(one, three));
    // A peer paired with itself is degenerate rather than an error, and must
    // still produce something stable rather than reading past its buffer.
    EXPECT_TRUE(pairDiscriminator(one, one) == pairDiscriminator(one, one));
}

JAMLINK_TEST(derivation_fails_closed_when_no_hmac_is_installed) {
    // Sending in the clear because key derivation was unavailable would be the
    // worst possible recovery, so there is deliberately no fallback path.
    jamlink::network::installHmacSha256(nullptr);
    EXPECT_TRUE(!jamlink::network::hmacAvailable());
    const auto secret = secretOf(7U);
    const auto local = prefixOf(1U);
    const auto remote = prefixOf(2U);
    std::array<std::uint8_t, 32U> key{};
    key.fill(0xAB);
    EXPECT_TRUE(!derivePeerKey(
        std::span<const std::uint8_t, 32U>(secret),
        std::span<const std::uint8_t, peerNoncePrefixBytes>(local),
        std::span<const std::uint8_t, peerNoncePrefixBytes>(remote),
        KeyDirection::HostToGuest, std::span<std::uint8_t, 32U>(key)));
    // And it wrote nothing, so a caller that ignored the result cannot end up
    // encrypting under a half-filled buffer.
    for (const std::uint8_t value : key) {
        EXPECT_TRUE(value == 0xAB);
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
              << " peer key schedule tests passed\n";
    return failures == 0U ? 0 : 1;
}
