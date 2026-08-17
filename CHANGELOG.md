# Changelog

## 0.4.1-test — 2026-08-17

- The room now shows one coherent answer about what is happening, rather than leaving the interface to reason across audio, network, transport and recording states itself. A session conductor gathers what each subsystem already knows and decides what it adds up to; the rule it enforces is that anything claiming a musician can play is gated on evidence that audio is actually moving, never on a socket existing. An authenticated peer proves two programs agree with each other, not that two people can hear each other.
- When something needs doing, JamLink now offers exactly one action rather than a wall of warnings, and separates what the musician must do from what JamLink is already handling. A clipping input matters, but not while the session cannot start at all, so it speaks only once nothing more important is wrong.

- Audio now travels Opus-encoded. Two streams drop from roughly 1.6 Mbit/s upstream to about 320 kbit/s including packet overhead: a 5 ms packet is 60 bytes where uncompressed PCM was 480. A loopback test over real sockets confirms Opus actually crosses the wire rather than the encoder having failed and the stream having fallen back silently.
- Every audio payload names the codec that produced it in its first byte, rather than both ends agreeing once and remembering. There is no state to disagree about, no window during which they can, and a peer changing codec mid-session is read correctly. Uncompressed remains a first-class path: it is what the impairment tests measure against, and what a machine falls back to if an encoder cannot be created, so a failure there costs bandwidth rather than silence.
- Added an Opus encoder and decoder, measured rather than assumed. At 96 kbit/s a 5 ms packet leaves the encoder as 60 bytes where uncompressed PCM takes 480, and the codec adds exactly 2.5 ms of delay: it runs in restricted low-delay mode, which disables the SILK layer and halves the delay the default mode would cost. Waveform correlation against the source is 0.9998, and 0.9997 measured after a burst of lost packets.
- Vendored libopus 1.4 rather than fetching it. The packaged corresponding-source archive is built with `git archive HEAD`, and libopus is statically linked, so fetching at configure time would leave that archive incomplete. 1.4 rather than 1.5 because 1.5's neural deep-PLC and DRED account for roughly 11 MB of tree that restricted low-delay mode can never reach. BSD-3-Clause with royalty-free patent grants, so unlike the ASIO SDK it also suits a future proprietary edition.

## 0.4.0-test — 2026-08-17

- Asserted ownership in the code. Ninety-eight JamLink-owned source files carried a licence identifier and no copyright notice at all, so nothing in the source said who owns it.
- Added a contributor agreement, and a pull request template that asks for it first. JamLink has one copyright holder, which is the only reason a differently-licensed edition remains possible later; a single contribution merged without an agreement would remove that permanently and could not be undone afterwards.
- Added a test that fails the build if the Steinberg ASIO SDK is included anywhere but the one file behind `ISoundcheckAudioService`. The SDK is dual-licensed and the proprietary option requires an agreement signed by Steinberg, so which licence JamLink ships under has to stay a licensing decision rather than becoming an architectural one. The test was verified to fail when the boundary is crossed, not merely to pass as written.
- Recorded in `docs/licensing-options.md` what the current dependencies actually allow: Qt is used only through modules available under LGPL and is deployed as replaceable DLLs, the icons are Apache 2.0, and the ASIO SDK is the one real gate on a closed-source edition.

- Outgoing audio is now taken from the capture callback at the capture device's own rate, instead of from the playback callback. Feeding the network from the playback side made what the other person hears a function of the local playback device: on Windows shared-mode audio the capture and render endpoints run on independent clocks with a converter between them, that converter zero-fills whatever it cannot supply, and its return value was discarded, so every underrun put a block of digital silence on the wire as though it were the guitar. ASIO was unaffected because capture and playback there share one clock in one callback, which is why the same session sounded acceptable in one direction and bit-crushed in the other.
- Rewrote the send schedule as a portable, clock-injected `OutgoingAudioPacer` and gave it a deterministic test suite. The previous schedule capped catch-up at four packets per wake-up and then rebased forward if still behind, which abandoned the audio behind the deficit; the backlog grew on every late wake-up until the converter overran and discarded in chunks. Lateness is now made up by sending sooner, never by moving the deadline, and the only audio deliberately dropped is a backlog older than a live session can use, which is counted.
- A read that cannot fill a whole packet is no longer attempted. The converter fills what it can, marks itself unprimed and loses the remainder, so with a wake-up coarser than one packet — which is what a five-millisecond timeout actually delivers on Windows — that was a steady, silent leak on every pass.
- Added an automatic buffer size, and made it the default for new installations. JamLink opens the device at the smallest size it offers and moves up only when that device reports that it dropped audio, so nobody has to guess — and guessing high is what makes playing together feel late. It never steps back down within a session, because a quiet stretch is not evidence the smaller size would have held. Settings reports the size actually in use, not just the word "Auto".
- Local audio drop-outs are now reported. The underrun and overrun counters had existed from the beginning and were shown nowhere, so "which buffer size should I use" had no answer but guesswork, and a device quietly failing to hand over audio in time looked exactly like a bad network. Settings now says how many blocks were dropped and that a larger buffer is the remedy, and the session log records the rate rather than a running total.
- Added mute switches for your own guitar and microphone in the room. Only the friend's channels had them, so there was no way to stop a vocal microphone sending guitar bleed. Muting a channel stops it reaching the other person while capture, meters, peak hold, clipping detection, the tuner and recording all continue, because every one of those sits upstream of the transport. This is a different control from the monitor switches, which only decide whether you hear yourself; both remain independent.
- Fixed the friend's mute switches doing nothing once a session had dropped and reconnected. The handler required the participant identifier to match the one the peer announced, and a session that drops clears that identifier until a fresh join event arrives, so after any blip the switch stayed enabled and inert: it moved, the guard rejected it, and the next refresh snapped it back. Muting a friend is a local playback decision and no longer depends on knowing who they are.
- A muted stream is now reported as muted rather than as silence. Muting sends no packets at all, so the receiving side could only say that nothing was arriving, which reads as a fault and sends both musicians hunting a connection problem that does not exist. The sender's per-stream mute state now travels on the periodic control packet — not on the audio packets that carry the clip flag, because those are exactly what a muted stream stops sending. Your friend's card names which of their channels they muted, and the connection line says so instead of "No audio from your friend".
- Your own meters keep moving while you are muted. Zeroing them is right for a friend's stream, which genuinely stopped arriving, but wrong for your own, where the input is still live and checking whether it is too hot is exactly why you muted.
- Fixed the tuner and the musician fighting over the same flag. The tuner's "mute while tuning" was the only writer of the outgoing instrument mute, so closing the tuner would have put a deliberately muted guitar back on the wire with nothing on screen changing to say so. Both intents are now combined by a single writer.
- A mute switch now restores its binding after being clicked, so a control can no longer show the last thing that was clicked instead of what is actually true.
- Settings is now reachable from inside a room, and returns to the room rather than dropping you at Home. Which device you are on is the setting most likely to need changing once you can hear the result, and leaving the session was previously the only way to reach it.
- Settings states what a change costs while two people are playing: levels, monitoring and mute take effect as you change them, while choosing a different input, output or buffer size restarts the audio device for a short silence. The session stays connected through it and no new invite is needed — the transport is untouched by an audio restart, and the backend re-applies the peer connection, tuner state and monitor controls every time it starts, so even moving from shared Windows audio to ASIO keeps your friend hearing you.
- A router that refuses to open a port now recommends that the other person create the invite, rather than explaining port forwarding. Only the host needs to be reachable, so swapping roles is the fix that needs no configuration; forwarding is still offered as the alternative.
- A router that rewrites the external port no longer reports "relay required" and withholds the invite with nothing further to offer. No relay exists in this build, and only the ability to host was ever lost: such a machine can still join a room the other person opens, and now says so.
- Settled three receive-path concerns that had been carried unresolved across several releases, and recorded the reasoning in `docs/receive-path.md` so they are not re-raised from the same reading of the code. Stale slots after a resync cannot strand a restarted stream, because a slot is released the moment it is played, so the ring only ever holds the live depth. A stream that stops still reports `playing`, which means "playout has started" and not "audio is arriving" — the receiver cannot answer the second question from depth alone, which is why the stall report is arrival-based and lives in the desktop. And a consumer that stalls does not carry its backlog forever, because the trim path walks playout back to the target. Two new regression tests hold the first and third.

## 0.3.9-test — 2026-08-15

- Made every audio device reachable in the picker. The device list sized itself to its content with no upper bound and the view inside was exactly as tall as that content, so there was nothing to scroll and every device below the bottom of the window was unreachable. On a machine with several interfaces and their ASIO drivers that was most of them, including ASIO entries that field testing found unreachable. The list is now bounded and scrollable, carries a draggable scrollbar, and opens with the current device in view.
- Recorded why an ASIO driver was skipped during enumeration. A driver that is installed but held by another application, or that does not offer 48 kHz, previously vanished from the list exactly as if it had never been installed, which gives a musician no way to tell "install the driver" from "close the other application".
- Added per-stream packet counters to the session summary, so a total that happens to equal one stream's worth can no longer be mistaken for a lossy link.

## 0.3.8-test — 2026-08-15

- Paced outgoing audio to the cadence it represents instead of releasing up to four packets back to back. An early two-home field test ran over a 4 ms round trip yet reported a 135 ms receive buffer and harsh, bit-crushed audio: bursts of packets made the receiver measure its own sender as a jittery network, so the buffer grew to absorb variance the link never had while concealment ran almost continuously. Packets now leave on a media-clock schedule, with bounded catch-up after a late wake-up and a rebase if the capture side gets too far ahead.

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
