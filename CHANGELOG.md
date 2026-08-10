# Changelog

## Unreleased

### Added

- Adaptive receive jitter buffer with reorder tolerance, duplicate and late rejection, bounded latency growth, and pitch-synchronous packet-loss concealment, implemented in the core library so it is testable without sockets or a GUI.
- Deterministic network impairment harness covering latency, jitter, random and burst loss, reordering, and duplication, with before/after measurements for transport changes.
- Independent instrument and voice network streams, each with its own sequence space, receive buffer, remote level, remote mute, and telemetry.
- Connection quality summary that separates the measured round trip and receive buffer depth from the estimated one-way delay.
- Peer transport tests for packet reflection, malformed datagram flooding, stream independence, and leave/rejoin key re-derivation.
- Room screen visual capture test.

### Changed

- Wire protocol version 2. Peers must run the same build.
- Each direction now uses its own AES-256-GCM key, derived from the room secret with HMAC-SHA256 under a distinct label, and packets carry an authenticated direction byte.
- Anti-replay uses a 64 entry sliding window over the per-direction nonce counter instead of a strictly increasing sequence, so genuinely reordered packets reach the jitter buffer.
- Nonce prefixes widened to eight bytes and the counter refuses to wrap.
- Round trip timing uses QueryPerformanceCounter rather than GetTickCount64, whose 10 to 16 ms resolution was coarser than the delays being reported.
- Instrument and voice are no longer summed and attenuated by 0.65 each before transmission.
- Output rate conversion on the remote path is bypassed when the device already runs at 48 kHz.
- Sliders, switches, and focus rings use the indigo accent already used elsewhere; level meters are green throughout.

### Fixed

- A UDP datagram larger than the receive buffer returned WSAEMSGSIZE, which was treated as a fatal socket error and ended the session. Anyone who knew the port could stop a jam with a single packet, without the room secret. ICMP port-unreachable could do the same through WSAECONNRESET.
- A peer's own authenticated packets, reflected back from its pinned endpoint, decrypted and played, because both directions shared one key.
- Reordered audio packets were discarded outright by the monotonic sequence check.
- Visual tests silently ran against the developer's real display and GPU: `set_tests_properties` read the second `ENVIRONMENT_MODIFICATION` entry as a property name, dropping the offscreen and software-rasteriser settings.

### Previously added

- Direct Windows WASAPI Shared endpoint enumeration, independent guitar/microphone capture, selected output, local monitoring, live meters, and output test.
- Allocation-free asynchronous clock conversion for independent capture, render, and remote-network rates.
- Functional encrypted `JL1` host/invite/join system for two direct IPv4 peers.
- AES-256-GCM authenticated UDP handshake/audio, replay rejection, endpoint pinning, room mute, packet telemetry, remote level, and network RTT.
- UPnP same-port mapping, Cloudflare STUN public-address discovery, and an exact manual UDP-forward fallback.
- Room UI and an automated real-loopback bidirectional encrypted audio test.
- Reproducible self-contained Windows friend-test package script, SHA-256 manifest, test guide, and source/license handoff.

- GPL-3.0-or-later project license, SPDX source notices, and separable interoperability licensing policy.
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
- User-facing build, test-session, dependency, licensing, and release documentation.
