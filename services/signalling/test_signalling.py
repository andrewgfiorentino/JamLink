# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

"""What the rendezvous owes two musicians, and what it must never learn."""

from __future__ import annotations

import unittest

from jamlink_signalling import (
    MAX_PARTICIPANTS_PER_SESSION,
    SignallingStore,
    sanitise_candidates,
)

TOKEN = "a" * 40
OTHER_TOKEN = "b" * 40
ALICE = "1" * 32
BOB = "2" * 32


def candidate(address: str = "203.0.113.7", port: int = 41234, kind: str = "host") -> dict:
    return {"address": address, "port": port, "kind": kind}


class RendezvousTests(unittest.TestCase):
    def test_two_clients_learn_each_other_and_not_themselves(self) -> None:
        store = SignallingStore()
        store.publish(TOKEN, ALICE, [candidate("192.168.1.20", 41234)], now=0.0)
        store.publish(TOKEN, BOB, [candidate("198.51.100.9", 55100)], now=0.1)

        for_alice = store.collect(TOKEN, ALICE, now=0.2)
        self.assertEqual(len(for_alice), 1)
        self.assertEqual(for_alice[0]["address"], "198.51.100.9")
        # Handing a client its own address back would have it probing itself.
        self.assertNotIn("192.168.1.20", [item["address"] for item in for_alice])

    def test_sessions_do_not_leak_into_each_other(self) -> None:
        store = SignallingStore()
        store.publish(TOKEN, ALICE, [candidate("203.0.113.7")], now=0.0)
        store.publish(OTHER_TOKEN, BOB, [candidate("198.51.100.9")], now=0.0)
        self.assertEqual(store.collect(OTHER_TOKEN, ALICE, now=0.1)[0]["address"],
                         "198.51.100.9")
        self.assertEqual(store.collect(TOKEN, BOB, now=0.1)[0]["address"], "203.0.113.7")

    def test_entries_expire_so_a_stale_address_is_never_handed_out(self) -> None:
        store = SignallingStore(ttl_seconds=60.0)
        store.publish(TOKEN, ALICE, [candidate()], now=0.0)
        self.assertEqual(len(store.collect(TOKEN, BOB, now=30.0)), 1)
        self.assertEqual(store.collect(TOKEN, BOB, now=120.0), [])
        self.assertEqual(store.session_count(), 0)

    def test_a_session_cannot_be_filled_without_bound(self) -> None:
        store = SignallingStore()
        for index in range(MAX_PARTICIPANTS_PER_SESSION):
            self.assertTrue(
                store.publish(TOKEN, f"{index:032d}", [candidate()], now=0.0))
        self.assertFalse(store.publish(TOKEN, "f" * 32, [candidate()], now=0.0))

    def test_republishing_replaces_rather_than_accumulates(self) -> None:
        # A client whose address changes mid-negotiation must not leave the old
        # one behind for the other side to keep probing.
        store = SignallingStore()
        store.publish(TOKEN, ALICE, [candidate("192.168.1.20", 41234)], now=0.0)
        store.publish(TOKEN, ALICE, [candidate("192.168.1.44", 41234)], now=1.0)
        gathered = store.collect(TOKEN, BOB, now=1.1)
        self.assertEqual(len(gathered), 1)
        self.assertEqual(gathered[0]["address"], "192.168.1.44")


class CandidateValidationTests(unittest.TestCase):
    def test_well_formed_candidates_are_accepted(self) -> None:
        cleaned = sanitise_candidates(
            [candidate(), candidate("fe80::1", 5000, "server-reflexive")])
        self.assertIsNotNone(cleaned)
        self.assertEqual(len(cleaned), 2)

    def test_anything_that_is_not_an_address_is_refused(self) -> None:
        for bad in (
            [{"address": "evil.example.com", "port": 80, "kind": "host"}],
            [{"address": "203.0.113.7", "port": 0, "kind": "host"}],
            [{"address": "203.0.113.7", "port": 70000, "kind": "host"}],
            [{"address": "203.0.113.7", "port": 41234, "kind": "smuggled"}],
            [{"address": "203.0.113.7", "port": "41234", "kind": "host"}],
            ["not-a-candidate"],
            {"not": "a list"},
        ):
            self.assertIsNone(sanitise_candidates(bad), bad)

    def test_extra_fields_cannot_ride_along(self) -> None:
        # An allowlist, not a filter: the service must not become a way to pass
        # arbitrary content between two clients under cover of connecting them.
        cleaned = sanitise_candidates(
            [{"address": "203.0.113.7", "port": 41234, "kind": "host",
              "secret": "room-key", "note": "chat message"}])
        self.assertIsNotNone(cleaned)
        self.assertEqual(set(cleaned[0]), {"address", "port", "kind"})
        self.assertNotIn("secret", cleaned[0])

    def test_a_flood_of_candidates_is_truncated(self) -> None:
        cleaned = sanitise_candidates([candidate() for _ in range(500)])
        self.assertIsNotNone(cleaned)
        self.assertLessEqual(len(cleaned), 12)


if __name__ == "__main__":
    unittest.main()
