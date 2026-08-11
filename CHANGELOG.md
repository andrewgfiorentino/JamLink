# Changelog

## 0.3.0-test — 2026-08-11

- Added native Steinberg ASIO SDK integration for Windows instrument capture and output.
- Added first-class mixed ASIO/WASAPI operation: ASIO remains the master instrument/output clock while a separate WASAPI microphone is synchronized through a bounded asynchronous bridge.
- Added safe secondary-microphone disconnect, reconnect, and replacement behavior without restarting the ASIO stream.
- Added persistent musician profiles, eight built-in musician avatars, and bounded custom-avatar sanitization.
- Added authenticated participant identity and exact application/build/media/control protocol matching.
- Added encrypted reliable room chat with acknowledgement, retry, deduplication, UTF-8 validation, size bounds, and rate limits.
- Added a GitHub Releases updater with streamed SHA-256 verification and a separate replacement/restart helper.
- Added the standalone JamLink directory/presence service and deterministic API tests; it is not exposed in this private-room desktop tester.
- Expanded automated tests for mixed-clock resampling, virtual one/eight-hour drift, concurrent capture/render, source replacement, network abuse, reconnect, updater replacement, persistence, and GUI rendering.

## 0.2.0-test

- Added independent instrument and voice streams, adaptive jitter buffering, pitch-synchronous packet-loss concealment, tuner, aligned four-track recording, and hardened encrypted direct-UDP sessions.

## 0.1.0-test

- Added the initial Windows Sound Check, encrypted two-person invite room, persistent settings, Qt desktop shell, and reproducible test package.
