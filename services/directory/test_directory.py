# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from jamlink_directory import DirectoryState


class FakeClock:
    def __init__(self) -> None:
        self.value = 1_000.0

    def __call__(self) -> float:
        return self.value

    def advance(self, seconds: float) -> None:
        self.value += seconds


class DirectoryStateTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.clock = FakeClock()
        self.state = DirectoryState(
            Path(self.temporary.name) / "directory.sqlite3", self.clock
        )

    def tearDown(self) -> None:
        self.state.close()
        self.temporary.cleanup()

    def register(self, handle: str, display_name: str) -> tuple[str, str]:
        response = self.state.register_profile(
            {
                "handle": handle,
                "display_name": display_name,
                "avatar_id": "avatar:guitar-electric",
                "primary_instrument": "Guitar",
            },
            "",
        )
        self.assertEqual(response.status, 201)
        return (
            str(response.payload["profile"]["profile_id"]),
            str(response.payload["credential"]),
        )

    def test_profile_identity_is_stable_and_handles_are_unique(self) -> None:
        profile_id, credential = self.register("andrewf", "Andrew")
        duplicate = self.state.register_profile(
            {
                "handle": "andrewf",
                "display_name": "Impostor",
                "avatar_id": "avatar:listener",
            },
            "",
        )
        self.assertEqual(duplicate.status, 409)
        updated = self.state.register_profile(
            {
                "handle": "andrew-guitar",
                "display_name": "Andrew",
                "avatar_id": "avatar:guitar-electric",
            },
            "Bearer " + credential,
        )
        self.assertEqual(updated.status, 200)
        self.assertEqual(updated.payload["profile"]["profile_id"], profile_id)

    def test_lobby_hides_invite_until_authenticated_compatible_join(self) -> None:
        host_id, _ = self.register("hostplayer", "Host")
        guest_id, _ = self.register("guestplayer", "Guest")
        lobby = self.state.register_lobby(
            host_id,
            {
                "name": "Delaware Test Jam",
                "description": "Two guitars",
                "genre": "Rock",
                "skill": "All levels",
                "region": "US East",
                "mode": "performer",
                "maximum_participants": 2,
                "application_version": "0.3.0",
                "build_identity": "a" * 40,
                "release_channel": "test",
                "media_protocol": 2,
                "control_protocol": 1,
                "invite_code": "JL1|203.0.113.2|45000|" + "a" * 64,
            },
        )
        self.assertEqual(lobby.status, 201)
        lobby_id = str(lobby.payload["lobby_id"])
        public = self.state.list_lobbies().payload["lobbies"][0]
        self.assertNotIn("invite_code", public)
        incompatible = self.state.join_lobby(
            guest_id,
            lobby_id,
            {
                "application_version": "0.2.0",
                "build_identity": "b" * 40,
                "release_channel": "test",
                "media_protocol": 2,
                "control_protocol": 1,
            },
        )
        self.assertEqual(incompatible.status, 409)
        compatible = self.state.join_lobby(
            guest_id,
            lobby_id,
            {
                "application_version": "0.3.0",
                "build_identity": "a" * 40,
                "release_channel": "test",
                "media_protocol": 2,
                "control_protocol": 1,
            },
        )
        self.assertEqual(compatible.status, 200)
        self.assertTrue(str(compatible.payload["invite_code"]).startswith("JL1|"))

    def test_presence_and_lobbies_expire_without_database_heartbeat_writes(self) -> None:
        profile_id, _ = self.register("clocktest", "Clock Test")
        self.assertEqual(
            self.state.update_presence(profile_id, {"state": "online"}).status, 200
        )
        self.clock.advance(46.0)
        self.assertEqual(self.state.online_count(), 0)

    def test_validation_and_rate_limit_are_bounded(self) -> None:
        invalid = self.state.register_profile(
            {"handle": "admin", "display_name": "A", "avatar_id": "avatar:listener"},
            "",
        )
        self.assertEqual(invalid.status, 400)
        for _ in range(120):
            self.assertTrue(self.state.rate_allowed("192.0.2.1"))
        self.assertFalse(self.state.rate_allowed("192.0.2.1"))


if __name__ == "__main__":
    unittest.main()
