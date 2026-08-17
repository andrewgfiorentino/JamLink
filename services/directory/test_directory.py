# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

from __future__ import annotations

import tempfile
import unittest
from pathlib import Path

from jamlink_directory import DirectoryServer, DirectoryState, Request


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

    def test_public_lobby_never_returns_bearer_media_invite(self) -> None:
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
        self.assertEqual(compatible.status, 403)
        self.assertEqual(compatible.payload, {"error": "host_admission_required"})
        self.assertNotIn("invite_code", compatible.payload)

    def test_presence_and_lobbies_expire_without_database_heartbeat_writes(self) -> None:
        profile_id, _ = self.register("clocktest", "Clock Test")
        self.assertEqual(
            self.state.update_presence(profile_id, {"state": "online"}).status, 200
        )
        self.clock.advance(46.0)
        self.assertEqual(self.state.online_count(), 0)

    def test_private_invite_code_is_ephemeral_unlisted_and_requires_admission(self) -> None:
        room_payload = {
            "code": "thewonderyears",
            "invite_code": "JL1|203.0.113.2|45000|" + "b" * 64,
            "application_version": "0.3.4",
            "build_identity": "c" * 40,
            "release_channel": "test",
            "media_protocol": 2,
            "control_protocol": 1,
        }
        created = self.state.create_private_room(room_payload)
        self.assertEqual(created.status, 201)
        self.assertEqual(created.payload["code"], "thewonderyears")
        owner_token = str(created.payload["owner_token"])

        self.assertEqual(self.state.list_lobbies().payload["lobbies"], [])
        self.assertNotIn("THEWONDERYEARS", self.state.list_lobbies().payload)
        self.assertEqual(self.state.create_private_room(room_payload).status, 409)

        requested = self.state.request_private_room(
            "THEWONDERYEARS",
            {
                "application_version": "0.3.4",
                "build_identity": "c" * 40,
                "release_channel": "test",
                "media_protocol": 2,
                "control_protocol": 1,
                "display_name": "Mike",
                "primary_instrument": "Bass",
                "avatar_id": "avatar:bass",
            },
        )
        self.assertEqual(requested.status, 201)
        self.assertNotIn("invite_code", requested.payload)
        request_id = str(requested.payload["request_id"])
        request_token = str(requested.payload["request_token"])

        waiting = self.state.private_request_status(
            request_id, "Bearer " + request_token
        )
        self.assertEqual(waiting.payload, {"status": "waiting"})
        self.assertEqual(
            self.state.list_private_requests("THEWONDERYEARS", "Bearer " + "0" * 64).status,
            403,
        )
        pending = self.state.list_private_requests(
            "THEWONDERYEARS", "Bearer " + owner_token
        )
        self.assertEqual(pending.status, 200)
        self.assertEqual(pending.payload["requests"][0]["display_name"], "Mike")
        second = self.state.request_private_room(
            "THEWONDERYEARS",
            {
                "application_version": "0.3.4",
                "build_identity": "c" * 40,
                "release_channel": "test",
                "media_protocol": 2,
                "control_protocol": 1,
                "display_name": "Chris",
            },
        )
        self.assertEqual(second.status, 201)
        second_id = str(second.payload["request_id"])
        second_token = str(second.payload["request_token"])

        self.assertEqual(
            self.state.decide_private_request(
                "THEWONDERYEARS", request_id, "Bearer " + request_token, True
            ).status,
            403,
        )
        admitted = self.state.decide_private_request(
            "THEWONDERYEARS", request_id, "Bearer " + owner_token, True
        )
        self.assertEqual(admitted.payload, {"status": "admitted"})
        resolved = self.state.private_request_status(
            request_id, "Bearer " + request_token
        )
        self.assertTrue(str(resolved.payload["invite_code"]).startswith("JL1|"))
        self.assertEqual(
            self.state.private_request_status(
                second_id, "Bearer " + second_token
            ).payload,
            {"status": "denied"},
        )
        self.assertEqual(
            self.state.decide_private_request(
                "THEWONDERYEARS", second_id, "Bearer " + owner_token, True
            ).status,
            409,
        )
        rejected_after_admission = self.state.request_private_room(
            "THEWONDERYEARS",
            {
                "application_version": "0.3.4",
                "build_identity": "c" * 40,
                "release_channel": "test",
                "media_protocol": 2,
                "control_protocol": 1,
            },
        )
        self.assertEqual(rejected_after_admission.status, 409)
        self.assertEqual(
            rejected_after_admission.payload, {"error": "room_already_admitted"}
        )
        self.assertEqual(
            self.state.heartbeat_private_room(
                "THEWONDERYEARS", "Bearer " + "0" * 64
            ).status,
            403,
        )
        heartbeat = self.state.heartbeat_private_room(
            "THEWONDERYEARS", "Bearer " + owner_token
        )
        self.assertEqual(heartbeat.status, 200)
        self.assertTrue(heartbeat.payload["admission_closed"])

    def test_private_waiting_list_has_a_hard_cap(self) -> None:
        payload = {
            "code": "CAPPED-ROOM",
            "invite_code": "JL1|198.51.100.8|45003|" + "a" * 64,
            "application_version": "0.3.4",
            "build_identity": "b" * 40,
            "release_channel": "test",
            "media_protocol": 2,
            "control_protocol": 1,
        }
        self.assertEqual(self.state.create_private_room(payload).status, 201)
        compatibility = {
            "application_version": "0.3.4",
            "build_identity": "b" * 40,
            "release_channel": "test",
            "media_protocol": 2,
            "control_protocol": 1,
        }
        for index in range(16):
            response = self.state.request_private_room(
                "CAPPED-ROOM", {**compatibility, "display_name": f"Guest {index}"}
            )
            self.assertEqual(response.status, 201)
        overflow = self.state.request_private_room("CAPPED-ROOM", compatibility)
        self.assertEqual(overflow.status, 429)
        self.assertEqual(overflow.payload, {"error": "waiting_room_full"})

    def test_private_room_and_waiting_request_expire_without_public_listing(self) -> None:
        created = self.state.create_private_room({
            "code": "BAND-PRACTICE",
            "invite_code": "JL1|198.51.100.7|45001|" + "d" * 64,
            "application_version": "0.3.4",
            "build_identity": "e" * 40,
            "release_channel": "test",
            "media_protocol": 2,
            "control_protocol": 1,
        })
        self.assertEqual(created.status, 201)
        requested = self.state.request_private_room(
            "BAND-PRACTICE",
            {
                "application_version": "0.3.4",
                "build_identity": "e" * 40,
                "release_channel": "test",
                "media_protocol": 2,
                "control_protocol": 1,
            },
        )
        self.assertEqual(requested.status, 201)
        self.clock.advance(91.0)
        self.assertTrue(self.state.expire())
        self.assertEqual(self.state.private_rooms, {})
        self.assertEqual(self.state.private_requests, {})
        reused = self.state.create_private_room({
            "code": "band-practice",
            "invite_code": "JL1|198.51.100.7|45001|" + "e" * 64,
            "application_version": "0.3.4",
            "build_identity": "e" * 40,
            "release_channel": "test",
            "media_protocol": 2,
            "control_protocol": 1,
        })
        self.assertEqual(reused.status, 201)
        self.assertEqual(reused.payload["code"], "band-practice")

    def test_private_invite_code_validation_case_matching_and_display(self) -> None:
        compatibility = {
            "application_version": "0.3.4",
            "build_identity": "9" * 40,
            "release_channel": "test",
            "media_protocol": 2,
            "control_protocol": 1,
        }
        invite = "JL1|198.51.100.9|45009|" + "9" * 64
        code = "Andrew_Mike"
        created = self.state.create_private_room({
            **compatibility, "code": code, "invite_code": invite
        })
        self.assertEqual(created.status, 201)
        self.assertEqual(created.payload["code"], code)
        self.assertEqual(
            self.state.create_private_room({
                **compatibility, "code": "andrew_mike", "invite_code": invite
            }).status,
            409,
        )
        self.assertEqual(
            self.state.request_private_room("ANDREW_MIKE", compatibility).status,
            201,
        )
        maximum = "A_" + "B" * 62
        self.assertEqual(len(maximum), 64)
        self.assertEqual(
            self.state.create_private_room({
                **compatibility, "code": maximum, "invite_code": invite
            }).status,
            201,
        )
        for invalid in ("abc", "A" * 65, "has space", "has.dot", "JAMLINK"):
            self.assertEqual(
                self.state.create_private_room({
                    **compatibility, "code": invalid, "invite_code": invite
                }).status,
                400,
            )

    def test_private_room_http_routes_never_reveal_invite_before_admission(self) -> None:
        server = DirectoryServer(self.state)
        compatibility = {
            "application_version": "0.3.4",
            "build_identity": "f" * 40,
            "release_channel": "test",
            "media_protocol": 2,
            "control_protocol": 1,
        }

        def route(
            method: str,
            target: str,
            payload: dict[str, object] | None = None,
            token: str = "",
        ):
            import json

            headers = {"authorization": "Bearer " + token} if token else {}
            return server.route(Request(
                method,
                target,
                headers,
                json.dumps(payload or {}).encode("utf-8"),
                "192.0.2.22",
            ))[0]

        created = route("POST", "/v1/private-rooms", {
            **compatibility,
            "code": "THEWONDERYEARS",
            "invite_code": "JL1|203.0.113.40|45002|" + "1" * 64,
        })
        self.assertEqual(created.status, 201)
        owner_token = str(created.payload["owner_token"])
        requested = route(
            "POST",
            "/v1/private-rooms/THEWONDERYEARS/requests",
            {**compatibility, "display_name": "Mike"},
        )
        self.assertEqual(requested.status, 201)
        self.assertNotIn("invite_code", requested.payload)
        request_id = str(requested.payload["request_id"])
        request_token = str(requested.payload["request_token"])
        self.assertEqual(
            route("GET", f"/v1/private-requests/{request_id}", token=request_token).payload,
            {"status": "waiting"},
        )
        self.assertEqual(
            route(
                "POST",
                f"/v1/private-rooms/THEWONDERYEARS/requests/{request_id}/admit",
                token=request_token,
            ).status,
            403,
        )
        self.assertEqual(
            route(
                "POST",
                f"/v1/private-rooms/THEWONDERYEARS/requests/{request_id}/admit",
                token=owner_token,
            ).status,
            200,
        )
        admitted = route(
            "GET", f"/v1/private-requests/{request_id}", token=request_token
        )
        self.assertTrue(str(admitted.payload["invite_code"]).startswith("JL1|"))
        self.assertEqual(route("GET", "/v1/lobbies").payload["lobbies"], [])
        self.assertEqual(
            route("DELETE", "/v1/private-rooms/THEWONDERYEARS", token=request_token).status,
            403,
        )
        self.assertEqual(
            route("DELETE", "/v1/private-rooms/THEWONDERYEARS", token=owner_token).status,
            204,
        )
        self.assertEqual(self.state.private_rooms, {})
        self.assertEqual(self.state.private_requests, {})

    def test_validation_and_rate_limit_are_bounded(self) -> None:
        invalid = self.state.register_profile(
            {"handle": "admin", "display_name": "A", "avatar_id": "avatar:listener"},
            "",
        )
        self.assertEqual(invalid.status, 400)
        for _ in range(120):
            self.assertTrue(self.state.rate_allowed("192.0.2.1"))
        self.assertFalse(self.state.rate_allowed("192.0.2.1"))

    def test_trusted_proxy_rate_limits_clients_independently(self) -> None:
        server = DirectoryServer(self.state, {"127.0.0.1"})
        for _ in range(120):
            for client in ("198.51.100.10", "198.51.100.11"):
                response, _ = server.route(Request(
                    "GET",
                    "/v1/health",
                    {"x-forwarded-for": client},
                    b"",
                    "127.0.0.1",
                ))
                self.assertEqual(response.status, 200)
        limited, _ = server.route(Request(
            "GET", "/v1/health", {"x-forwarded-for": "198.51.100.10"},
            b"", "127.0.0.1",
        ))
        self.assertEqual(limited.status, 429)

        untrusted = DirectoryServer(self.state, set())
        untrusted_response, _ = untrusted.route(Request(
            "GET", "/v1/health", {"x-forwarded-for": "203.0.113.90"},
            b"", "203.0.113.1",
        ))
        self.assertEqual(untrusted_response.status, 200)
        self.assertIn("203.0.113.1", self.state.rate_windows)

        invalid_forwarded, _ = server.route(Request(
            "GET", "/v1/health", {"x-forwarded-for": "not-an-address"},
            b"", "127.0.0.1",
        ))
        self.assertEqual(invalid_forwarded.status, 200)
        self.assertIn("127.0.0.1", self.state.rate_windows)

    def test_event_stream_endpoint_is_disabled(self) -> None:
        unauthenticated, _ = DirectoryServer(self.state).route(Request(
            "GET", "/v1/events", {}, b"", "192.0.2.50"
        ))
        self.assertEqual(unauthenticated.status, 404)
        _profile_id, credential = self.register("eventtest", "Event Test")
        response, _ = DirectoryServer(self.state).route(Request(
            "GET", "/v1/events", {"authorization": "Bearer " + credential},
            b"", "192.0.2.50"
        ))
        self.assertEqual(response.status, 404)

    def test_expired_rate_limit_identities_are_reclaimed(self) -> None:
        for index in range(1_000):
            self.assertTrue(self.state.rate_allowed(f"198.51.{index // 256}.{index % 256}"))
        self.assertEqual(len(self.state.rate_windows), 1_000)
        self.clock.advance(61.0)
        self.state.expire()
        self.assertEqual(self.state.rate_windows, {})

    def test_host_and_guest_polling_share_one_nat_without_throttling(self) -> None:
        server = DirectoryServer(self.state)
        shared_peer = "198.51.100.77"
        for poll in range(36):
            requests_this_tick = 3 if poll % 12 == 0 else 2
            for _ in range(requests_this_tick):
                response, _ = server.route(Request(
                    "GET", "/v1/health", {}, b"", shared_peer
                ))
                self.assertEqual(response.status, 200)
            self.clock.advance(2.0)


if __name__ == "__main__":
    unittest.main()
