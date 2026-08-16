# Changelog

## 0.3.8-test — 2026-08-15

- Paced outgoing audio to the cadence it represents instead of releasing up to four packets back to back. The first successful two-home session ran over a 4 ms round trip yet reported a 135 ms receive buffer and harsh, bit-crushed audio: bursts of packets made the receiver measure its own sender as a jittery network, so the buffer grew to absorb variance the link never had while concealment ran almost continuously. Packets now leave on a media-clock schedule, with bounded catch-up after a late wake-up and a rebase if the capture side gets too far ahead.

## 0.3.7-test — 2026-08-15

- Buffer size choices now read as, for example, `128 · 5.3 ms · lowest delay`, with a plain-language note saying how long it takes to hear yourself and what to do if the audio crackles. The figure is the round trip a player feels, one buffer in and one buffer out, computed against the rate actually in use.
- Added a **Hearing yourself** line reporting the real monitor delay, and on Windows shared audio stating that Windows will not go lower and naming the two remedies: choose an ASIO device, or turn JamLink's monitor off and use the interface's own monitoring.
- Left sample rate as a single value on purpose. JamLink records, sends, and plays at 48 kHz, so any other rate would add a conversion stage and increase delay rather than reduce it, and on Windows shared audio the rate is not JamLink's to choose. The setting now explains that instead of offering a control that cannot help.
- Stopped printing a monitor delay of `0.0 ms` before the device has reported the buffer it settled on, which read as instant.

## 0.3.6-test — 2026-08-15

- Added in-application Windows Firewall detection and repair. A router mapping only carries a packet as far as the PC; the firewall then decides whether JamLink receives it, and for an unsigned application run from a ZIP the answer is usually no while every other check still reports success. Home now states the problem and offers **Fix Firewall**, which requests elevation through the standard UAC prompt and creates the narrowest useful rule: this executable, inbound, UDP, allow, private and domain profiles.
- Firewall repair distinguishes no rule, a rule left pointing at an older copy of the executable after an update or a move, a network marked Public, and a firewall that is simply off. Earlier JamLink rules are removed first so repairs cannot accumulate duplicates. The firewall is never disabled and rules are never widened to other programs or ports. A network marked Public is explained rather than silently opened.
- Added per-input monitor switches for guitar and voice in the room. Turning one off silences only JamLink's own monitor mix while capture, meters, peak hold, clipping detection, the tuner tap, recording, and what the other person hears all continue, so an interface with direct hardware monitoring can be used on its own near-zero-latency path.
- Moved the tuner into a panel beside the room instead of a separate screen, so meters, participants, and chat stay visible. It mutes the guitar to the room by default and can be left open while playing once that is switched off.
- Recorded the audio backend, requested and running buffer size, sample rate, and resulting monitor round trip in the session log, so a report of delayed monitoring can be answered from evidence.

## 0.3.5-test — 2026-08-15

- Added PCP and NAT-PMP port mapping alongside UPnP. With a single invite code the host cannot learn the guest's endpoint first, so a working router mapping is the only thing that makes it reachable; many routers ship one of these three with the others disabled or broken.
- Added a local session log recording the connection lifecycle: port binding, each mapping protocol tried and its answer, public-address discovery, whether the router rewrote the port, and a five-second summary while a connection stalls that distinguishes "nothing has arrived at all" from "packets arrive but the handshake has not completed". The room secret is never written and the log never leaves the machine.
- Added `jamlink_net_probe`, which reports what this network actually allows and names Windows Firewall explicitly, since a router mapping only delivers a packet as far as the PC and every other check still looks healthy when the firewall drops it.
- The host now punches toward a known guest endpoint while reconnecting, so a session that drops does not need a new invite when the guest's router lets its mapping expire.
- Recorded that a router which rewrites the external port is symmetric, and that a direct invite cannot work through it.

## 0.3.4-test — 2026-08-14

- Made the clipping meters themselves clickable to clear a latched warning, so input gain can be adjusted and rechecked without hunting for the small indicator button; the meter shows a Reset affordance on hover and only accepts the click while a latch is actually set.
- Fixed the connection grade reporting a silent link as better than a working one. A stream that stops arriving parks playout past its last packet, so the buffer reads zero and nothing is booked as concealment; the room now states plainly that no audio is arriving instead of upgrading the grade.
- Fixed the loss rate being divided by every datagram the transport received, including the other stream and control traffic, which understated instrument loss by roughly half and reported one grade band better than reality.
- Fixed the grade going deaf over a long session: it is now measured over a trailing eight-second window rather than session totals, so a fresh dropout still moves it after an hour of clean play.
- Stopped presenting an unmeasured round trip of zero as a measurement; the transport now reports whether a pong has actually returned.
- Restored the receive-path design note, and recorded the three reporting rules that these defects each broke.
- Added a pre-host connection check for Sound Check readiness, local build/protocol identity, UDP binding, public-address discovery, and automatic router mapping.
- Added clear **Ready**, **Direct connection may need help**, and fail-safe action states while keeping the full encrypted direct invite available when optional Internet preparation is inconclusive.
- Clarified that reachability is inferred rather than measured and that a friend’s exact build is verified during the encrypted join.

## 0.3.3-test — 2026-08-13

- Added a silent end-to-end clipping-indicator self-test and a latched near-full-scale risk detector for native inputs that have effectively no remaining headroom.
- Reserved the meter's final red segment for a real latched condition so a hot signal no longer looks clipped before the detector fires.
- Added optional temporary 4-64 character private invite-code support with case-insensitive matching, random generation, session expiry/reuse, unlisted rendezvous, and explicit host admission; controls remain hidden in packages without a configured rendezvous endpoint.
- Preserved the full `JL1` direct-invite path whenever temporary-code rendezvous is unavailable.
- Hid the direct invitation payload behind a prominent **Copy Invite** action while preserving paste-to-join compatibility.
- Expanded callback allocation/reset/priority tests, directory validation/expiry/admission tests, and minimum-width GUI coverage.

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
