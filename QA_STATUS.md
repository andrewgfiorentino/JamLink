# QA status

## Current gate

The provisional engine/GUI foundation remains accepted. Version 0.2.0 adds a testable WASAPI Shared plus encrypted two-peer direct-UDP vertical slice. The Windows local-audio, Internet reliability, security, and public-release gates remain open; this is a limited friend-test candidate.

## Current automated evidence

- clean MSVC Debug and Release builds with `/W4 /WX` on JamLink sources;
- deterministic core tests, allocation trap, clock/resampler simulations, and accelerated stress;
- encrypted host/guest invite handshake and bidirectional nonzero PCM exchange on real loopback UDP sockets;
- controller/persistence/readiness tests;
- deterministic offscreen Home, Sound Check, and Settings captures plus exact 150% dimensions;
- clean typed-QML lint;
- real Windows WASAPI Shared endpoint enumeration/open observed separately on development hardware;
- reproducible self-contained deployment script with per-file SHA-256 manifest.

These results do not claim live-user, two-home, subjective audio, firewall/router compatibility, production security, public package compliance, hardware latency, LAN latency, or WAN latency validation.

Final local candidate run: 24/24 Release core cases, 8/8 Debug CTests, 8/8 Release CTests, 20/20 repeated encrypted peer-loopback runs, clean typed-QML lint, and a one-hour simulated core stress run at 1064.98x synthetic throughput with zero reported ring underruns/overruns. The final ZIP manifest verified and an extracted copy launched without developer paths and opened the development machine's WASAPI Shared combination.

## Veto conditions

Reject any candidate with an audio-path allocation/blocking operation, possible Private Soundcheck transmission, channel corruption, uncontrolled drift, unauthenticated packet acceptance, invite leakage, misleading measurement, fake control, failing deterministic test, fictional documentation, or out-of-scope committed content.
