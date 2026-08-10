# Changelog

## Unreleased

### Added

- Direct Windows WASAPI Shared endpoint enumeration, independent guitar/microphone capture, selected output, local monitoring, live meters, and output test.
- Allocation-free asynchronous clock conversion for independent capture, render, and remote-network rates.
- Functional encrypted `JL1` host/invite/join system for two direct IPv4 peers.
- AES-256-GCM authenticated UDP handshake/audio, replay rejection, endpoint pinning, room mute, packet telemetry, remote level, and network RTT.
- UPnP same-port mapping, Cloudflare STUN public-address discovery, and an exact manual UDP-forward fallback.
- Room UI and an automated real-loopback bidirectional encrypted audio test.
- Reproducible self-contained Windows friend-test package script, SHA-256 manifest, test guide, and source/license handoff.

- GPL-3.0-or-later project license, SPDX source notices, and separable interoperability licensing policy.
- Direct-ASIO and Qt 6 GPLv3 dependency directions with exact-ingestion gates.
- Pinned Qt 6.10.3/QML desktop shell under Qt's GPL-3.0-only option.
- Native Home, Private Sound Check, Audio Settings, and functional two-person Room screens aligned to the approved visual reference; tuner/record/chat controls remain absent until implemented.
- Versioned, validated, atomically replaced preferences with first/second-launch restoration by stable device/channel identifiers.
- Deterministic GUI controller tests, offscreen high-DPI visual captures, and a clean QML lint target.
- Vendored Material Design Icons Round assets with exact revision, hashes, modification notice, and Apache-2.0 license.
- C++20/CMake build and Windows presets.
- Initial audio-device backend contract with explicit channel selection and capability reporting.
- Preallocated channel/bus route graph.
- Lock-free SPSC interleaved audio ring with underrun/overrun telemetry.
- Smoothed gain/mute, peak/RMS/clip metering, and invalid-sample containment.
- Local-only Private Soundcheck processor.
- Readiness invalidation and safe join-mute decisions.
- Clock-drift estimator and bounded occupancy controller.
- Deterministic simulated-device, integrity, allocation, hot-plug, virtual-time, and stress tests.
- Project status, architecture, real-time, test, benchmark, dependency, security, and limitation documentation.
