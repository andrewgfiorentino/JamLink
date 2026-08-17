# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

"""Rendezvous so two JamLink clients can punch a hole at the same moment.

This service exists because hole punching needs both ends to send to each other
simultaneously, and neither can learn the other's addresses on its own. It
carries a few hundred bytes per session and then gets out of the way. No audio
ever touches it.

What it deliberately does not know:

  The room secret. Clients derive a rendezvous token by hashing their invite,
  so this service sees an opaque identifier and could not decrypt a session
  even if it were compromised or subpoenaed.

  Who anyone is. No accounts, no names, no profile identifiers.

  Anything, for long. Entries expire in minutes and are held in memory only,
  so there is no database to leak and nothing survives a restart.

It is small on purpose: it fits any free tier, needs no UDP, and if it is down
JamLink still works exactly as it does today via the direct invite.
"""

from __future__ import annotations

import json
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any

# Long enough to survive a musician reading out a code, short enough that a
# stale address is never handed to anyone.
SESSION_TTL_SECONDS = 300.0
# One session is two people. More than a handful of publishes means something
# is wrong, and bounding it keeps memory flat under abuse.
MAX_PARTICIPANTS_PER_SESSION = 8
MAX_CANDIDATES_PER_PARTICIPANT = 12
MAX_SESSIONS = 5_000
MAX_BODY_BYTES = 8 * 1024

# A rendezvous token is a hex digest, never an invite.
TOKEN_PATTERN = re.compile(r"^[0-9a-f]{32,64}$")
# Addresses only. Anything else is refused rather than stored and echoed.
ADDRESS_PATTERN = re.compile(r"^[0-9a-fA-F:.]{3,45}$")
VALID_KINDS = {"host", "server-reflexive", "relayed"}


class SignallingStore:
    """In-memory rendezvous with a time-to-live.

    Deliberately not a database. There is nothing here worth persisting, and
    anything persisted would be worth protecting.
    """

    def __init__(self, ttl_seconds: float = SESSION_TTL_SECONDS) -> None:
        self._ttl = ttl_seconds
        self._lock = threading.Lock()
        self._sessions: dict[str, dict[str, Any]] = {}

    def _expire(self, now: float) -> None:
        stale = [token for token, entry in self._sessions.items()
                 if now - entry["updated"] > self._ttl]
        for token in stale:
            del self._sessions[token]

    def publish(
        self, token: str, participant: str, candidates: list[dict], now: float
    ) -> bool:
        with self._lock:
            self._expire(now)
            entry = self._sessions.get(token)
            if entry is None:
                if len(self._sessions) >= MAX_SESSIONS:
                    return False
                entry = {"updated": now, "participants": {}}
                self._sessions[token] = entry
            participants = entry["participants"]
            if (participant not in participants
                    and len(participants) >= MAX_PARTICIPANTS_PER_SESSION):
                return False
            participants[participant] = candidates[:MAX_CANDIDATES_PER_PARTICIPANT]
            entry["updated"] = now
            return True

    def collect(self, token: str, participant: str, now: float) -> list[dict]:
        """Everyone else's candidates. Never the caller's own back again."""
        with self._lock:
            self._expire(now)
            entry = self._sessions.get(token)
            if entry is None:
                return []
            gathered: list[dict] = []
            for other, candidates in entry["participants"].items():
                if other == participant:
                    continue
                gathered.extend(candidates)
            return gathered

    def session_count(self) -> int:
        with self._lock:
            return len(self._sessions)


def sanitise_candidates(raw: Any) -> list[dict] | None:
    """Accept only well-formed candidates.

    An allowlist rather than a filter: a field that is not named here cannot be
    stored, so this service cannot be used to pass arbitrary content between
    two clients under the guise of connecting them.
    """
    if not isinstance(raw, list):
        return None
    cleaned: list[dict] = []
    for item in raw[:MAX_CANDIDATES_PER_PARTICIPANT]:
        if not isinstance(item, dict):
            return None
        address = item.get("address")
        port = item.get("port")
        kind = item.get("kind")
        if not isinstance(address, str) or not ADDRESS_PATTERN.match(address):
            return None
        if not isinstance(port, int) or not (1 <= port <= 65_535):
            return None
        if kind not in VALID_KINDS:
            return None
        cleaned.append({"address": address, "port": port, "kind": kind})
    return cleaned


class SignallingHandler(BaseHTTPRequestHandler):
    store: SignallingStore = SignallingStore()
    server_version = "JamLinkSignalling/1"

    def log_message(self, *_args: Any) -> None:
        """Silence per-request logging.

        Request logs would record which addresses talked to which, which is
        exactly the correlation this service is designed not to hold.
        """

    def _respond(self, code: int, payload: dict) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/health":
            self._respond(200, {"ok": True, "sessions": self.store.session_count()})
            return
        self._respond(404, {"error": "not found"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/rendezvous":
            self._respond(404, {"error": "not found"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._respond(400, {"error": "bad length"})
            return
        if length <= 0 or length > MAX_BODY_BYTES:
            self._respond(413, {"error": "body too large"})
            return
        try:
            request = json.loads(self.rfile.read(length).decode("utf-8"))
        except (ValueError, UnicodeDecodeError):
            self._respond(400, {"error": "bad json"})
            return
        if not isinstance(request, dict):
            self._respond(400, {"error": "bad request"})
            return

        token = request.get("token")
        participant = request.get("participant")
        if not isinstance(token, str) or not TOKEN_PATTERN.match(token):
            self._respond(400, {"error": "bad token"})
            return
        if not isinstance(participant, str) or not TOKEN_PATTERN.match(participant):
            self._respond(400, {"error": "bad participant"})
            return

        candidates = sanitise_candidates(request.get("candidates", []))
        if candidates is None:
            self._respond(400, {"error": "bad candidates"})
            return

        now = time.monotonic()
        if not self.store.publish(token, participant, candidates, now):
            self._respond(429, {"error": "session full"})
            return
        # Publishing and collecting in one exchange is what lets both sides
        # start probing within a round trip of each other, which is what makes
        # simultaneous punching possible at all.
        self._respond(200, {"candidates": self.store.collect(token, participant, now)})


def serve(host: str = "0.0.0.0", port: int = 8787) -> None:
    ThreadingHTTPServer((host, port), SignallingHandler).serve_forever()


if __name__ == "__main__":
    serve()
