# SPDX-License-Identifier: GPL-3.0-or-later
"""Small self-hostable JamLink profile, presence, and lobby directory."""

from __future__ import annotations

import argparse
import asyncio
import collections
import dataclasses
import hashlib
import ipaddress
import json
import re
import secrets
import sqlite3
import time
import uuid
from pathlib import Path
from typing import Any

MAX_HEADER_BYTES = 16 * 1024
MAX_BODY_BYTES = 64 * 1024
PRESENCE_TTL_SECONDS = 45.0
LOBBY_TTL_SECONDS = 45.0
PRIVATE_ROOM_TTL_SECONDS = 90.0
PRIVATE_REQUEST_TTL_SECONDS = 120.0
MAXIMUM_PRIVATE_REQUESTS_PER_ROOM = 16
HANDLE_PATTERN = re.compile(r"^[a-z0-9][a-z0-9_.-]{2,23}$")
PRIVATE_ROOM_CODE_PATTERN = re.compile(r"^[A-Z0-9][A-Z0-9-]{3,31}$")
RESERVED_HANDLES = {"admin", "administrator", "jamlink", "moderator", "support", "system"}
RESERVED_ROOM_CODES = {"ADMIN", "JAMLINK", "MODERATOR", "SUPPORT", "SYSTEM"}


@dataclasses.dataclass(slots=True)
class Request:
    method: str
    target: str
    headers: dict[str, str]
    body: bytes
    peer: str


@dataclasses.dataclass(slots=True)
class Response:
    status: int
    payload: dict[str, Any] | list[Any]


@dataclasses.dataclass(slots=True)
class Presence:
    profile_id: str
    state: str
    expires_at: float


@dataclasses.dataclass(slots=True)
class Lobby:
    lobby_id: str
    host_profile_id: str
    name: str
    description: str
    genre: str
    skill: str
    region: str
    mode: str
    maximum_participants: int
    participant_count: int
    application_version: str
    build_identity: str
    release_channel: str
    media_protocol: int
    control_protocol: int
    invite_code: str
    expires_at: float


@dataclasses.dataclass(slots=True)
class PrivateRoom:
    code: str
    invite_code: str
    owner_token_hash: str
    application_version: str
    build_identity: str
    release_channel: str
    media_protocol: int
    control_protocol: int
    admission_closed: bool
    expires_at: float


@dataclasses.dataclass(slots=True)
class PrivateJoinRequest:
    request_id: str
    room_code: str
    request_token_hash: str
    display_name: str
    primary_instrument: str
    avatar_id: str
    status: str
    expires_at: float


def _clean_text(value: Any, maximum: int, *, required: bool = False) -> str:
    if not isinstance(value, str):
        raise ValueError("text field has the wrong type")
    cleaned = " ".join(value.replace("\r", " ").replace("\n", " ").split())
    if len(cleaned) > maximum or (required and not cleaned):
        raise ValueError("text field has an invalid length")
    return cleaned


def _json_bytes(payload: dict[str, Any] | list[Any]) -> bytes:
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


class DirectoryState:
    def __init__(self, database_path: Path, clock: Any = time.monotonic) -> None:
        self.clock = clock
        self.database_path = database_path
        self.database_path.parent.mkdir(parents=True, exist_ok=True)
        self.database = sqlite3.connect(database_path)
        self.database.row_factory = sqlite3.Row
        self.database.executescript(
            """
            PRAGMA journal_mode=WAL;
            CREATE TABLE IF NOT EXISTS profiles (
                profile_id TEXT PRIMARY KEY,
                handle TEXT NOT NULL UNIQUE,
                display_name TEXT NOT NULL,
                avatar_id TEXT NOT NULL,
                primary_instrument TEXT NOT NULL,
                genres TEXT NOT NULL,
                bio TEXT NOT NULL,
                region TEXT NOT NULL,
                share_region INTEGER NOT NULL,
                credential_hash TEXT NOT NULL,
                updated_at INTEGER NOT NULL
            );
            CREATE TABLE IF NOT EXISTS reports (
                report_id TEXT PRIMARY KEY,
                reporter_profile_id TEXT NOT NULL,
                target_profile_id TEXT NOT NULL,
                reason TEXT NOT NULL,
                created_at INTEGER NOT NULL
            );
            """
        )
        self.database.commit()
        self.presences: dict[str, Presence] = {}
        self.lobbies: dict[str, Lobby] = {}
        # Private room names are deliberately ephemeral and never written to
        # SQLite or returned by the public lobby endpoint. The human-readable
        # code is a rendezvous alias; the high-entropy JL1 media secret stays
        # withheld until the host explicitly admits a request.
        self.private_rooms: dict[str, PrivateRoom] = {}
        self.private_requests: dict[str, PrivateJoinRequest] = {}
        self.rate_windows: dict[str, collections.deque[float]] = {}

    def close(self) -> None:
        self.database.close()

    def rate_allowed(self, peer: str, limit: int = 120) -> bool:
        now = self.clock()
        window = self.rate_windows.setdefault(peer, collections.deque())
        while window and window[0] < now - 60.0:
            window.popleft()
        if len(window) >= limit:
            return False
        window.append(now)
        return True

    def authenticate(self, authorization: str) -> str | None:
        if not authorization.startswith("Bearer "):
            return None
        credential = authorization[7:]
        if len(credential) != 64 or not all(character in "0123456789abcdef" for character in credential):
            return None
        digest = hashlib.sha256(credential.encode("ascii")).hexdigest()
        row = self.database.execute(
            "SELECT profile_id, credential_hash FROM profiles WHERE credential_hash = ?",
            (digest,),
        ).fetchone()
        return str(row["profile_id"]) if row is not None else None

    def profile_public(self, profile_id: str) -> dict[str, Any] | None:
        row = self.database.execute(
            "SELECT profile_id, handle, display_name, avatar_id, primary_instrument, "
            "genres, bio, region, share_region FROM profiles WHERE profile_id = ?",
            (profile_id,),
        ).fetchone()
        if row is None:
            return None
        return {
            "profile_id": row["profile_id"],
            "handle": row["handle"],
            "display_name": row["display_name"],
            "avatar_id": row["avatar_id"],
            "primary_instrument": row["primary_instrument"],
            "genres": row["genres"],
            "bio": row["bio"],
            "region": row["region"] if row["share_region"] else "",
        }

    def register_profile(self, payload: dict[str, Any], authorization: str) -> Response:
        handle = _clean_text(payload.get("handle", ""), 24, required=True).casefold()
        if not HANDLE_PATTERN.fullmatch(handle) or handle in RESERVED_HANDLES:
            return Response(400, {"error": "handle_invalid"})
        try:
            fields = {
                "display_name": _clean_text(payload.get("display_name", ""), 48, required=True),
                "avatar_id": _clean_text(payload.get("avatar_id", "avatar:listener"), 48, required=True),
                "primary_instrument": _clean_text(payload.get("primary_instrument", ""), 48),
                "genres": _clean_text(payload.get("genres", ""), 96),
                "bio": _clean_text(payload.get("bio", ""), 280),
                "region": _clean_text(payload.get("region", ""), 64),
            }
        except ValueError:
            return Response(400, {"error": "profile_invalid"})
        existing_profile = self.authenticate(authorization)
        now = int(time.time())
        if existing_profile is not None:
            collision = self.database.execute(
                "SELECT profile_id FROM profiles WHERE handle = ? AND profile_id <> ?",
                (handle, existing_profile),
            ).fetchone()
            if collision is not None:
                return Response(409, {"error": "handle_taken"})
            self.database.execute(
                "UPDATE profiles SET handle=?, display_name=?, avatar_id=?, "
                "primary_instrument=?, genres=?, bio=?, region=?, share_region=?, updated_at=? "
                "WHERE profile_id=?",
                (
                    handle, fields["display_name"], fields["avatar_id"],
                    fields["primary_instrument"], fields["genres"], fields["bio"],
                    fields["region"], 1 if payload.get("share_region", False) else 0,
                    now, existing_profile,
                ),
            )
            self.database.commit()
            return Response(200, {"profile": self.profile_public(existing_profile)})

        credential = secrets.token_hex(32)
        profile_id = str(uuid.uuid4())
        try:
            self.database.execute(
                "INSERT INTO profiles VALUES (?,?,?,?,?,?,?,?,?,?,?)",
                (
                    profile_id, handle, fields["display_name"], fields["avatar_id"],
                    fields["primary_instrument"], fields["genres"], fields["bio"],
                    fields["region"], 1 if payload.get("share_region", False) else 0,
                    hashlib.sha256(credential.encode("ascii")).hexdigest(), now,
                ),
            )
            self.database.commit()
        except sqlite3.IntegrityError:
            return Response(409, {"error": "handle_taken"})
        return Response(
            201,
            {"profile": self.profile_public(profile_id), "credential": credential},
        )

    def update_presence(self, profile_id: str, payload: dict[str, Any]) -> Response:
        state = payload.get("state", "online")
        if state not in {"online", "hosting", "jamming", "listening"}:
            return Response(400, {"error": "presence_invalid"})
        self.presences[profile_id] = Presence(
            profile_id, state, self.clock() + PRESENCE_TTL_SECONDS
        )
        return Response(200, {"online_count": self.online_count()})

    def online_count(self) -> int:
        self.expire()
        return len(self.presences)

    def register_lobby(self, profile_id: str, payload: dict[str, Any]) -> Response:
        try:
            name = _clean_text(payload.get("name", ""), 48, required=True)
            description = _clean_text(payload.get("description", ""), 240)
            genre = _clean_text(payload.get("genre", "Any genre"), 32, required=True)
            skill = _clean_text(payload.get("skill", "All levels"), 32, required=True)
            region = _clean_text(payload.get("region", ""), 64)
            mode = _clean_text(payload.get("mode", "performer"), 16, required=True)
            application_version = _clean_text(
                payload.get("application_version", ""), 32, required=True
            )
            build_identity = _clean_text(payload.get("build_identity", ""), 64, required=True)
            release_channel = _clean_text(payload.get("release_channel", ""), 16, required=True)
            invite_code = _clean_text(payload.get("invite_code", ""), 2048, required=True)
            media_protocol = int(payload.get("media_protocol", 0))
            control_protocol = int(payload.get("control_protocol", 0))
            maximum = int(payload.get("maximum_participants", 2))
        except (ValueError, TypeError):
            return Response(400, {"error": "lobby_invalid"})
        if mode not in {"performer", "listen"} or not 2 <= maximum <= 12 \
                or not invite_code.startswith("JL1|") \
                or not 1 <= media_protocol <= 65_535 \
                or not 1 <= control_protocol <= 65_535:
            return Response(400, {"error": "lobby_invalid"})
        for lobby_id, lobby in list(self.lobbies.items()):
            if lobby.host_profile_id == profile_id:
                del self.lobbies[lobby_id]
        lobby_id = str(uuid.uuid4())
        self.lobbies[lobby_id] = Lobby(
            lobby_id, profile_id, name, description, genre, skill, region, mode,
            maximum, 1, application_version, build_identity, release_channel,
            media_protocol, control_protocol, invite_code,
            self.clock() + LOBBY_TTL_SECONDS,
        )
        self.presences[profile_id] = Presence(
            profile_id, "hosting", self.clock() + PRESENCE_TTL_SECONDS
        )
        return Response(201, {"lobby_id": lobby_id})

    def lobby_public(self, lobby: Lobby) -> dict[str, Any]:
        return {
            "lobby_id": lobby.lobby_id,
            "name": lobby.name,
            "description": lobby.description,
            "genre": lobby.genre,
            "skill": lobby.skill,
            "region": lobby.region,
            "mode": lobby.mode,
            "maximum_participants": lobby.maximum_participants,
            "participant_count": lobby.participant_count,
            "application_version": lobby.application_version,
            "build_identity": lobby.build_identity,
            "release_channel": lobby.release_channel,
            "media_protocol": lobby.media_protocol,
            "control_protocol": lobby.control_protocol,
            "host_profile": self.profile_public(lobby.host_profile_id),
        }

    def list_lobbies(self) -> Response:
        self.expire()
        return Response(200, {
            "online_count": len(self.presences),
            "lobbies": [self.lobby_public(lobby) for lobby in self.lobbies.values()],
        })

    def heartbeat_lobby(self, profile_id: str, lobby_id: str) -> Response:
        lobby = self.lobbies.get(lobby_id)
        if lobby is None:
            return Response(404, {"error": "lobby_not_found"})
        if lobby.host_profile_id != profile_id:
            return Response(403, {"error": "not_host"})
        lobby.expires_at = self.clock() + LOBBY_TTL_SECONDS
        self.presences[profile_id] = Presence(
            profile_id, "hosting", self.clock() + PRESENCE_TTL_SECONDS
        )
        return Response(200, {"ok": True})

    def remove_lobby(self, profile_id: str, lobby_id: str) -> Response:
        lobby = self.lobbies.get(lobby_id)
        if lobby is None:
            return Response(204, {})
        if lobby.host_profile_id != profile_id:
            return Response(403, {"error": "not_host"})
        del self.lobbies[lobby_id]
        return Response(204, {})

    def join_lobby(self, profile_id: str, lobby_id: str, payload: dict[str, Any]) -> Response:
        del profile_id
        self.expire()
        lobby = self.lobbies.get(lobby_id)
        if lobby is None:
            return Response(404, {"error": "lobby_not_found"})
        compatible = (
            payload.get("application_version") == lobby.application_version
            and payload.get("build_identity") == lobby.build_identity
            and payload.get("release_channel") == lobby.release_channel
            and payload.get("media_protocol") == lobby.media_protocol
            and payload.get("control_protocol") == lobby.control_protocol
        )
        if not compatible:
            return Response(409, {
                "error": "update_required",
                "required_version": lobby.application_version,
            })
        # Public discovery never conveys the bearer media invite. Public-room
        # waiting/admission will issue participant-bound authorization; until
        # that path is active, joining fails closed instead of leaking JL1.
        return Response(403, {"error": "host_admission_required"})

    @staticmethod
    def private_token_matches(authorization: str, expected_hash: str) -> bool:
        if not authorization.startswith("Bearer "):
            return False
        token = authorization[7:]
        if len(token) != 64 or not all(character in "0123456789abcdef" for character in token):
            return False
        return secrets.compare_digest(
            hashlib.sha256(token.encode("ascii")).hexdigest(), expected_hash
        )

    @staticmethod
    def normalize_private_code(value: Any) -> str:
        if not isinstance(value, str):
            raise ValueError("room code has the wrong type")
        code = value.strip().upper()
        if not PRIVATE_ROOM_CODE_PATTERN.fullmatch(code) or code in RESERVED_ROOM_CODES:
            raise ValueError("room code is invalid")
        return code

    def create_private_room(
        self, payload: dict[str, Any], authorization: str = ""
    ) -> Response:
        self.expire()
        try:
            code = self.normalize_private_code(payload.get("code", ""))
            invite_code = _clean_text(payload.get("invite_code", ""), 2048, required=True)
            application_version = _clean_text(
                payload.get("application_version", ""), 32, required=True
            )
            build_identity = _clean_text(payload.get("build_identity", ""), 64, required=True)
            release_channel = _clean_text(
                payload.get("release_channel", ""), 16, required=True
            )
            media_protocol = int(payload.get("media_protocol", 0))
            control_protocol = int(payload.get("control_protocol", 0))
        except (ValueError, TypeError):
            return Response(400, {"error": "private_room_invalid"})
        if not invite_code.startswith("JL1|") \
                or not 1 <= media_protocol <= 65_535 \
                or not 1 <= control_protocol <= 65_535:
            return Response(400, {"error": "private_room_invalid"})

        existing = self.private_rooms.get(code)
        if existing is not None and not self.private_token_matches(
            authorization, existing.owner_token_hash
        ):
            return Response(409, {"error": "room_code_taken"})
        owner_token = secrets.token_hex(32)
        self.private_rooms[code] = PrivateRoom(
            code,
            invite_code,
            hashlib.sha256(owner_token.encode("ascii")).hexdigest(),
            application_version,
            build_identity,
            release_channel,
            media_protocol,
            control_protocol,
            False,
            self.clock() + PRIVATE_ROOM_TTL_SECONDS,
        )
        for request_id, request in list(self.private_requests.items()):
            if request.room_code == code:
                del self.private_requests[request_id]
        return Response(201, {
            "code": code,
            "owner_token": owner_token,
            "expires_in_seconds": int(PRIVATE_ROOM_TTL_SECONDS),
        })

    def heartbeat_private_room(self, code: str, authorization: str) -> Response:
        self.expire()
        room = self.private_rooms.get(code)
        if room is None:
            return Response(404, {"error": "private_room_not_found"})
        if not self.private_token_matches(authorization, room.owner_token_hash):
            return Response(403, {"error": "not_room_owner"})
        room.expires_at = self.clock() + PRIVATE_ROOM_TTL_SECONDS
        return Response(200, {
            "ok": True,
            "admission_closed": room.admission_closed,
        })

    def remove_private_room(self, code: str, authorization: str) -> Response:
        self.expire()
        room = self.private_rooms.get(code)
        if room is None:
            return Response(204, {})
        if not self.private_token_matches(authorization, room.owner_token_hash):
            return Response(403, {"error": "not_room_owner"})
        del self.private_rooms[code]
        self.private_requests = {
            key: value
            for key, value in self.private_requests.items()
            if value.room_code != code
        }
        return Response(204, {})

    def request_private_room(self, code: str, payload: dict[str, Any]) -> Response:
        self.expire()
        room = self.private_rooms.get(code)
        if room is None:
            return Response(404, {"error": "private_room_not_found"})
        if room.admission_closed:
            return Response(409, {"error": "room_already_admitted"})
        compatible = (
            payload.get("application_version") == room.application_version
            and payload.get("build_identity") == room.build_identity
            and payload.get("release_channel") == room.release_channel
            and payload.get("media_protocol") == room.media_protocol
            and payload.get("control_protocol") == room.control_protocol
        )
        if not compatible:
            return Response(409, {
                "error": "update_required",
                "required_version": room.application_version,
            })
        try:
            display_name = _clean_text(
                payload.get("display_name", "Musician"), 48, required=True
            )
            primary_instrument = _clean_text(
                payload.get("primary_instrument", ""), 48
            )
            avatar_id = _clean_text(
                payload.get("avatar_id", "avatar:listener"), 48, required=True
            )
        except ValueError:
            return Response(400, {"error": "join_request_invalid"})
        pending_count = sum(
            1
            for request in self.private_requests.values()
            if request.room_code == code and request.status == "waiting"
        )
        if pending_count >= MAXIMUM_PRIVATE_REQUESTS_PER_ROOM:
            return Response(429, {"error": "waiting_room_full"})
        request_id = str(uuid.uuid4())
        request_token = secrets.token_hex(32)
        self.private_requests[request_id] = PrivateJoinRequest(
            request_id,
            code,
            hashlib.sha256(request_token.encode("ascii")).hexdigest(),
            display_name,
            primary_instrument,
            avatar_id,
            "waiting",
            self.clock() + PRIVATE_REQUEST_TTL_SECONDS,
        )
        return Response(201, {
            "request_id": request_id,
            "request_token": request_token,
            "status": "waiting",
        })

    def list_private_requests(self, code: str, authorization: str) -> Response:
        self.expire()
        room = self.private_rooms.get(code)
        if room is None:
            return Response(404, {"error": "private_room_not_found"})
        if not self.private_token_matches(authorization, room.owner_token_hash):
            return Response(403, {"error": "not_room_owner"})
        requests = [
            {
                "request_id": request.request_id,
                "display_name": request.display_name,
                "primary_instrument": request.primary_instrument,
                "avatar_id": request.avatar_id,
                "status": request.status,
            }
            for request in self.private_requests.values()
            if request.room_code == code and request.status == "waiting"
        ]
        return Response(200, {"requests": requests})

    def decide_private_request(
        self,
        code: str,
        request_id: str,
        authorization: str,
        approve: bool,
    ) -> Response:
        self.expire()
        room = self.private_rooms.get(code)
        request = self.private_requests.get(request_id)
        if room is None or request is None or request.room_code != code:
            return Response(404, {"error": "join_request_not_found"})
        if not self.private_token_matches(authorization, room.owner_token_hash):
            return Response(403, {"error": "not_room_owner"})
        if request.status != "waiting":
            return Response(409, {"error": "join_request_already_decided"})
        if approve and room.admission_closed:
            return Response(409, {"error": "room_already_admitted"})
        request.status = "admitted" if approve else "denied"
        request.expires_at = self.clock() + PRIVATE_REQUEST_TTL_SECONDS
        if approve:
            # JL1 is a single-performer bearer-key session. Atomically close
            # this room-name slot on first admission so the same media secret
            # cannot authorize a later requester after the active peer drops.
            room.admission_closed = True
            for other in self.private_requests.values():
                if other.room_code == code and other.request_id != request_id \
                        and other.status == "waiting":
                    other.status = "denied"
                    other.expires_at = self.clock() + PRIVATE_REQUEST_TTL_SECONDS
        return Response(200, {"status": request.status})

    def private_request_status(
        self, request_id: str, authorization: str
    ) -> Response:
        self.expire()
        request = self.private_requests.get(request_id)
        if request is None:
            return Response(404, {"error": "join_request_not_found"})
        if not self.private_token_matches(authorization, request.request_token_hash):
            return Response(403, {"error": "not_request_owner"})
        payload: dict[str, Any] = {"status": request.status}
        if request.status == "admitted":
            room = self.private_rooms.get(request.room_code)
            if room is None:
                return Response(404, {"error": "private_room_not_found"})
            payload["invite_code"] = room.invite_code
        return Response(200, payload)

    def report(self, reporter: str, payload: dict[str, Any]) -> Response:
        try:
            target = _clean_text(payload.get("target_profile_id", ""), 64, required=True)
            reason = _clean_text(payload.get("reason", ""), 280, required=True)
        except ValueError:
            return Response(400, {"error": "report_invalid"})
        if self.profile_public(target) is None or target == reporter:
            return Response(400, {"error": "report_invalid"})
        self.database.execute(
            "INSERT INTO reports VALUES (?,?,?,?,?)",
            (str(uuid.uuid4()), reporter, target, reason, int(time.time())),
        )
        self.database.commit()
        return Response(201, {"accepted": True})

    def expire(self) -> bool:
        now = self.clock()
        before = (
            len(self.presences), len(self.lobbies),
            len(self.private_rooms), len(self.private_requests),
        )
        self.presences = {
            key: value for key, value in self.presences.items() if value.expires_at > now
        }
        self.lobbies = {
            key: value for key, value in self.lobbies.items() if value.expires_at > now
        }
        self.private_rooms = {
            key: value
            for key, value in self.private_rooms.items()
            if value.expires_at > now
        }
        self.private_requests = {
            key: value
            for key, value in self.private_requests.items()
            if value.expires_at > now and value.room_code in self.private_rooms
        }
        for peer, window in list(self.rate_windows.items()):
            while window and window[0] < now - 60.0:
                window.popleft()
            if not window:
                del self.rate_windows[peer]
        return before != (
            len(self.presences), len(self.lobbies),
            len(self.private_rooms), len(self.private_requests),
        )


class DirectoryServer:
    def __init__(
        self, state: DirectoryState, trusted_proxies: set[str] | None = None
    ) -> None:
        self.state = state
        self.trusted_proxies = trusted_proxies or set()
        self.server: asyncio.AbstractServer | None = None
        self.cleanup_task: asyncio.Task[None] | None = None

    async def start(self, host: str, port: int) -> None:
        self.server = await asyncio.start_server(self.handle_connection, host, port)
        self.cleanup_task = asyncio.create_task(self.cleanup_loop())

    async def close(self) -> None:
        if self.cleanup_task is not None:
            self.cleanup_task.cancel()
            try:
                await self.cleanup_task
            except asyncio.CancelledError:
                pass
        if self.server is not None:
            self.server.close()
            await self.server.wait_closed()
        self.state.close()

    @property
    def port(self) -> int:
        assert self.server is not None
        return int(self.server.sockets[0].getsockname()[1])

    async def cleanup_loop(self) -> None:
        while True:
            await asyncio.sleep(5.0)
            self.state.expire()

    async def read_request(self, reader: asyncio.StreamReader, peer: str) -> Request:
        header = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), 10.0)
        if len(header) > MAX_HEADER_BYTES:
            raise ValueError("headers_too_large")
        lines = header[:-4].decode("iso-8859-1").split("\r\n")
        method, target, version = lines[0].split(" ", 2)
        if version != "HTTP/1.1":
            raise ValueError("http_version")
        headers: dict[str, str] = {}
        for line in lines[1:]:
            name, value = line.split(":", 1)
            headers[name.strip().lower()] = value.strip()
        length = int(headers.get("content-length", "0"))
        if length < 0 or length > MAX_BODY_BYTES:
            raise ValueError("body_too_large")
        body = await asyncio.wait_for(reader.readexactly(length), 10.0) if length else b""
        return Request(method, target.split("?", 1)[0], headers, body, peer)

    async def handle_connection(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> None:
        peer_info = writer.get_extra_info("peername")
        peer = str(peer_info[0]) if peer_info else "unknown"
        try:
            request = await self.read_request(reader, peer)
            response, _changed = self.route(request)
        except (ValueError, UnicodeError, asyncio.IncompleteReadError, asyncio.LimitOverrunError):
            response, changed = Response(400, {"error": "bad_request"}), False
        except asyncio.TimeoutError:
            response, changed = Response(408, {"error": "request_timeout"}), False
        await self.write_response(writer, response)

    def rate_identity(self, request: Request) -> str:
        if request.peer not in self.trusted_proxies:
            return request.peer
        forwarded = request.headers.get("x-forwarded-for", "").split(",", 1)[0].strip()
        try:
            return str(ipaddress.ip_address(forwarded))
        except ValueError:
            return request.peer

    def route(self, request: Request) -> tuple[Response, bool]:
        if not self.state.rate_allowed(self.rate_identity(request)):
            return Response(429, {"error": "rate_limited"}), False
        try:
            payload = json.loads(request.body.decode("utf-8")) if request.body else {}
        except (json.JSONDecodeError, UnicodeError):
            return Response(400, {"error": "invalid_json"}), False
        if not isinstance(payload, dict):
            return Response(400, {"error": "invalid_json"}), False
        authorization = request.headers.get("authorization", "")
        profile_id = self.state.authenticate(authorization)

        if request.target == "/v1/events":
            return Response(404, {"error": "not_found"}), False
        if request.method == "GET" and request.target == "/v1/health":
            return Response(200, {"service": "jamlink-directory", "version": 2}), False
        if request.method == "GET" and request.target == "/v1/lobbies":
            return self.state.list_lobbies(), False
        if request.method == "POST" and request.target == "/v1/private-rooms":
            response = self.state.create_private_room(payload, authorization)
            return response, False
        match = re.fullmatch(
            r"/v1/private-rooms/([A-Za-z0-9-]{4,32})/(heartbeat|requests)",
            request.target,
        )
        if match:
            try:
                room_code = self.state.normalize_private_code(match.group(1))
            except ValueError:
                return Response(400, {"error": "private_room_invalid"}), False
            action = match.group(2)
            if request.method == "POST" and action == "heartbeat":
                return self.state.heartbeat_private_room(room_code, authorization), False
            if request.method == "POST" and action == "requests":
                return self.state.request_private_room(room_code, payload), False
            if request.method == "GET" and action == "requests":
                return self.state.list_private_requests(room_code, authorization), False
        match = re.fullmatch(
            r"/v1/private-rooms/([A-Za-z0-9-]{4,32})/requests/"
            r"([0-9a-f-]{36})/(admit|deny)",
            request.target,
        )
        if request.method == "POST" and match:
            try:
                room_code = self.state.normalize_private_code(match.group(1))
            except ValueError:
                return Response(400, {"error": "private_room_invalid"}), False
            return self.state.decide_private_request(
                room_code,
                match.group(2),
                authorization,
                match.group(3) == "admit",
            ), False
        match = re.fullmatch(
            r"/v1/private-requests/([0-9a-f-]{36})", request.target
        )
        if request.method == "GET" and match:
            return self.state.private_request_status(match.group(1), authorization), False
        match = re.fullmatch(
            r"/v1/private-rooms/([A-Za-z0-9-]{4,32})", request.target
        )
        if request.method == "DELETE" and match:
            try:
                room_code = self.state.normalize_private_code(match.group(1))
            except ValueError:
                return Response(400, {"error": "private_room_invalid"}), False
            return self.state.remove_private_room(room_code, authorization), False
        if request.method == "POST" and request.target == "/v1/profiles/register":
            response = self.state.register_profile(payload, authorization)
            return response, response.status < 300
        if profile_id is None:
            return Response(401, {"error": "authentication_required"}), False
        if request.method == "POST" and request.target == "/v1/presence":
            return self.state.update_presence(profile_id, payload), True
        if request.method == "POST" and request.target == "/v1/lobbies":
            response = self.state.register_lobby(profile_id, payload)
            return response, response.status < 300
        if request.method == "POST" and request.target == "/v1/reports":
            return self.state.report(profile_id, payload), False
        match = re.fullmatch(r"/v1/lobbies/([0-9a-f-]{36})/(heartbeat|join)", request.target)
        if request.method == "POST" and match:
            lobby_id, action = match.groups()
            if action == "heartbeat":
                response = self.state.heartbeat_lobby(profile_id, lobby_id)
                return response, response.status < 300
            return self.state.join_lobby(profile_id, lobby_id, payload), False
        match = re.fullmatch(r"/v1/lobbies/([0-9a-f-]{36})", request.target)
        if request.method == "DELETE" and match:
            response = self.state.remove_lobby(profile_id, match.group(1))
            return response, response.status < 300
        return Response(404, {"error": "not_found"}), False

    async def write_response(self, writer: asyncio.StreamWriter, response: Response) -> None:
        body = b"" if response.status == 204 else _json_bytes(response.payload)
        reasons = {
            200: "OK", 201: "Created", 204: "No Content", 400: "Bad Request",
            401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
            408: "Request Timeout", 409: "Conflict", 429: "Too Many Requests",
        }
        header = (
            f"HTTP/1.1 {response.status} {reasons.get(response.status, 'Error')}\r\n"
            "Content-Type: application/json; charset=utf-8\r\n"
            f"Content-Length: {len(body)}\r\n"
            "Cache-Control: no-store\r\n"
            "Connection: close\r\n\r\n"
        ).encode("ascii")
        writer.write(header + body)
        await writer.drain()
        writer.close()
        await writer.wait_closed()

async def _run(arguments: argparse.Namespace) -> None:
    state = DirectoryState(Path(arguments.database))
    server = DirectoryServer(state, set(arguments.trusted_proxy))
    await server.start(arguments.host, arguments.port)
    print(f"JamLink directory listening on {arguments.host}:{server.port}", flush=True)
    try:
        await asyncio.Future()
    finally:
        await server.close()


def main() -> None:
    parser = argparse.ArgumentParser(description="JamLink directory and presence service")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8787)
    parser.add_argument("--database", default="data/jamlink-directory.sqlite3")
    parser.add_argument(
        "--trusted-proxy",
        action="append",
        default=[],
        help="Proxy IP allowed to supply X-Forwarded-For; repeat as needed",
    )
    arguments = parser.parse_args()
    try:
        asyncio.run(_run(arguments))
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
