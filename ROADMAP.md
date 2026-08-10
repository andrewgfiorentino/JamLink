# Roadmap

## Current - two-person Windows friend-test milestone

- [x] C++20/CMake foundation, warnings-as-errors, allocation traps, deterministic long-duration drift tests.
- [x] GPL-3.0-or-later repository and Qt 6.10.3 GPL-3.0-only desktop selection.
- [x] Native Home, Private Soundcheck, Settings, and Room screens from the approved visual direction.
- [x] Stable device/channel preferences and fail-safe readiness invalidation.
- [x] Direct WASAPI Shared endpoint enumeration, capture, render, local monitors, meters, and output test.
- [x] Preallocated asynchronous input/output/network clock conversion.
- [x] Explicit Private Soundcheck null-transport state and graph isolation.
- [x] Working host/create-invite and paste/join flow for exactly two peers.
- [x] AES-256-GCM authenticated direct UDP handshake/audio with replay rejection.
- [x] UPnP same-port mapping, STUN public-address discovery, and displayed manual-forward port.
- [x] Bidirectional encrypted loopback socket/audio automation.
- [x] Reproducible self-contained Windows friend-test ZIP script.
- [ ] Perform the first two-home live test and record only product-facing defects/results.

## Hardening gates

1. Add controlled loss/jitter/reorder simulation, mature adaptive jitter buffering, concealment, and reconnection.
2. Add authenticated relay/TURN so valid invites work behind CGNAT/symmetric NAT.
3. Replace PCM with a benchmarked low-delay codec while retaining a replaceable transport boundary.
4. Add device lifecycle/failure seams, race/leak instrumentation, and resampler quality matrices.
5. Ingest the exact official GPLv3 ASIO SDK archive and implement the direct ASIO backend.
6. Add independent security review, packet fuzzing, forward secrecy, and protocol version negotiation.
7. Mirror exact corresponding sources, finalize deployed notices, sign the executable/installer, and audit accessibility.

Tuner, temporary soundcheck recording, chat, larger rooms, recording, advanced routing, and DAW bridges follow only after the two-peer audio path is stable and measured.
