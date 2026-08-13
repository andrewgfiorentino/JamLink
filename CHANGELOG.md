# Changelog

## 0.3.2-test — 2026-08-13

- Moved update discovery to application launch and added a prominent in-app Update & Restart flow using the existing verified replacement helper.
- Rebuilt Home, Room, and Tuner around the approved dark premium visual direction and centralized reusable visual tokens.
- Fixed the Home Start/Join action cards, successful Sound Check navigation, and tuner return navigation for an active room.
- Added responsive data-driven participant cards with automated two- and four-musician layouts; an additional invite attempt cannot displace an active encrypted peer.
- Kept device selections, gains, monitor choices, profile, recording location, network preferences, and window placement persistent across restarts and updates.
- Expanded updater, controller, QML, high-DPI, multi-participant visual, and real-socket network regression coverage.

## 0.3.1-test — 2026-08-13

- Removed the extra Windows console from the packaged desktop application.
- Added compact native move, edge/corner resize, and close controls without adding a full title bar.
- A successful Sound Check save now returns directly to Home; failed verification remains on Sound Check with the real reason visible.
- Kept the encrypted reliable two-person chat path in the release gate and added an explicit compatibility test proving a `0.3.0-test` updater selects the `0.3.1-test` package.
- Made Windows package and corresponding-source filenames derive from the configured project version to reduce the chance of breaking future updates.

## 0.3.0-test — 2026-08-11

- Added native Steinberg ASIO SDK integration for Windows instrument capture and output.
- Added first-class mixed ASIO/WASAPI operation: ASIO remains the master instrument/output clock while a separate WASAPI microphone is synchronized through a bounded asynchronous bridge.
- Added safe secondary-microphone disconnect, reconnect, and replacement behavior without restarting the ASIO stream.
- Added independent latched input, send, monitor-mix, and recording-stage clipping diagnostics with peak hold, clip sample/event counters, stage-specific guidance, reset controls in Sound Check and Room, and authenticated per-stream remote clip reports.
- Added persistent musician profiles, eight built-in musician avatars, and bounded custom-avatar sanitization.
- Added authenticated participant identity and exact application/build/media/control protocol matching.
- Added encrypted reliable room chat with acknowledgement, retry, deduplication, UTF-8 validation, size bounds, and rate limits.
- Added a GitHub Releases updater with streamed SHA-256 verification and a separate replacement/restart helper.
- Added the standalone JamLink directory/presence service and deterministic API tests; it is not exposed in this private-room desktop tester.
- Expanded automated tests for mixed-clock resampling, virtual one/eight-hour drift, concurrent capture/render, clipping persistence/reset/independence/internal mix overload, network abuse, reconnect, updater replacement, persistence, and GUI rendering.

## 0.2.0-test

- Added independent instrument and voice streams, adaptive jitter buffering, pitch-synchronous packet-loss concealment, tuner, aligned four-track recording, and hardened encrypted direct-UDP sessions.

## 0.1.0-test

- Added the initial Windows Sound Check, encrypted two-person invite room, persistent settings, Qt desktop shell, and reproducible test package.
