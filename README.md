# JamLink

**Play music together over the internet, in real time.** JamLink is a free and
open-source Windows application for private two-person sessions: guitar and
voice travel as separate encrypted streams over a direct connection, with no
server in the middle and no account to create.

> Status: **0.4.4-test**. Under active development and validated in two-home
> field testing, but not yet at a 1.0 release. Expect rough edges.

## What it does

**Low latency is the whole point.** With an ASIO interface, monitoring runs at
around 5 ms and playing together feels like being in a room rather than
trading messages. Windows shared audio works too, at a higher floor Windows
imposes.

- **Direct encrypted audio.** AES-256-GCM with per-direction keys, a replay
  window, and authenticated participant identity. Audio goes peer to peer.
- **One code to connect.** The host creates an invite and sends it; nothing
  else is exchanged. The invite names every address the host can be reached on
  rather than one guess, and the guest probes all of them at once, keeping
  whichever answers. Two people in the same building stay on the local network
  instead of hairpinning through a router. Port mapping is still attempted
  automatically over UPnP, PCP, and NAT-PMP, and Windows Firewall is detected
  and repairable from inside the application.
- **Guitar and voice as separate streams.** Each has its own jitter buffer,
  level, and mute, so a friend's guitar can be turned down without turning
  their voice down.
- **Survives an imperfect connection.** An adaptive jitter buffer sized from
  measured arrival jitter, and pitch-synchronous packet-loss concealment that
  extrapolates the instrument's own waveform rather than inserting silence.
- **A chromatic tuner** that mutes your guitar to the room while you use it,
  and can stay open beside the session once you turn that off.
- **Recording only what you meant to record.** Each of the four sources can be
  left out of a take, and a source you turn off is never written to disk rather
  than written and discarded. A take says which sources were deliberately
  excluded, so a missing track can never be mistaken for one that failed.
- **Recording that keeps a pristine copy.** Your instrument and your voice, each
  of your friend's streams, and — separately — local originals of your own
  sources taken before the monitor converter and before the network could touch
  them. The live tracks are what was heard; the originals are what was played.
- **A take that opens.** Every finished take writes a session file beside the
  WAVs that lays each track on one timeline, named by who played it and whether
  it was heard, played, or received — so a recording is not six files to drag
  into place and hope.
- **Recordings that admit what they are.** A take carries a manifest saying what
  the files are, whose they are, and whether it actually finished. An
  interrupted take is never presented as clean.
- **A mute you cannot forget.** One press stops sending your guitar and voice,
  and while it is on the room says so plainly rather than leaving you to
  wonder whether your friend can hear you.
- **Private text chat** over the same authenticated connection.
- **A send that adapts to the uplink you actually have.** Each end reports what
  it is losing, and the other lowers its rate when that is sustained — quickly
  down, slowly back up, so a session never swings audibly between two
  qualities. A last-resort limiter keeps a hot input from reaching your friend
  as clipping, on the network path only: what you monitor, what you record, and
  the pristine originals are untouched.
- **Opus, tuned for playing rather than listening.** Restricted low-delay mode
  adds 2.5 ms, against the 6.5 ms the default mode would cost — more than the
  entire monitoring path on an ASIO interface. Two streams take roughly
  320 kbit/s upstream instead of 1.6 Mbit/s, and less than that on a connection
  that cannot carry it.
- **One answer, not a dashboard.** A session conductor gathers what every
  subsystem knows and says whether you can play. Anything claiming you can is
  gated on evidence that audio is actually moving, never on a socket existing.
  When something needs doing, JamLink offers one action rather than a wall of
  warnings.
- **Honest reporting.** Where a figure is inferred rather than measured, the
  text says so. Round trip and receive buffering are measured; one-way delay
  is derived.

## Installing

Download the latest ZIP from [Releases](../../releases), extract it to a normal
folder, and run `JamLink.exe`. Both people must run the **same version** — the
wire protocol is version-checked and mismatched builds refuse to join rather
than failing obscurely.

The build is not code-signed, so Windows will show a reputation warning. Verify
the published SHA-256 against the `.sha256` file before choosing **More info →
Run anyway**.

Requires Windows 11 x64 and headphones. There is no echo cancellation, by
design — it costs latency and colours the instrument.

See [docs/running-a-session.md](docs/running-a-session.md) for the two-person
setup walkthrough.

## Audio setup, briefly

**If you have an audio interface, select its ASIO driver** for input and
output. ASIO runs capture and playback on one clock in one callback. Windows
shared audio runs them on independent clocks with a converter between, which
costs both delay and quality.

Buffer size can be left on **Auto**: the device is opened at the smallest size
it offers and moves up only if it actually reports dropping audio.

Some combinations cannot run at all — an interface's own driver takes exclusive
ownership, so its input cannot be used while its output goes through Windows.
JamLink recognises this as you choose it, names the one device to change, and
offers a button that changes it.

If your interface does its own zero-latency monitoring, turn JamLink's monitor
off for that input and use the hardware path. Capture, meters, clipping
detection, the tuner, recording, and what your friend hears all continue.

## Known limitations

- **Windows only**, x64.
- **Two people per room.** A small group is the goal and the design is settled
  on a full mesh — everyone sending directly to everyone — but the transport
  still carries exactly one peer. See
  [docs/multi-participant-architecture.md](docs/multi-participant-architecture.md)
  for the plan and the bandwidth arithmetic behind it.
- **No relay, and no rendezvous yet.** The guest probes every address the
  invite named, but the host cannot punch outward until it has heard from the
  guest. If both ends are behind carrier-grade or symmetric NAT, a direct
  connection may still be impossible. JamLink says so rather than pretending,
  and will suggest the other person host if they can.
- **Exchanging originals is not implemented yet.** Each side keeps pristine
  local originals of its own sources, but they are not yet sent to the other
  person, so your copy of your friend's audio is still whatever arrived.
- **Both people must run the same version.** The wire protocol is
  version-checked, and it changed in 0.4.1. The invite format changed in 0.4.3,
  and 0.4.4 keys each pair of musicians separately rather than keying the room,
  so 0.4.4 and earlier builds cannot talk to each other at all.

## Building

Requirements: Windows 11 x64, CMake 3.25 or newer, Visual Studio 2022 with the
C++ toolchain, and Qt 6.10.3 for MSVC 2022 x64 at `.qt/6.10.3/msvc2022_64`.

The desktop application:

```powershell
cmake --preset windows-gui-vs2022
cmake --build --preset windows-gui-debug
ctest --preset windows-gui-debug
```

The portable core and its tests, without Qt:

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-debug
ctest --preset windows-debug
```

A distributable ZIP, including the corresponding source archive and licence
notices:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1
```

## How it is tested

Realtime audio breaks in ways that are hard to tell apart from a bad network,
so the test suite is built around making that distinction impossible to get
wrong.

- **Deterministic network impairment.** The receive path is driven through
  loss, reordering, duplication, and jitter models, with concealment quality
  measured against the source signal rather than eyeballed.
- **Conservation of audio.** The send path asserts that every captured frame
  was either transmitted, is still queued, or was counted as discarded, so
  audio cannot quietly go missing between the capture callback and the socket.
- **Real sockets.** Two transports handshake, exchange encrypted audio and
  chat, reconnect, and reject malformed and replayed packets over loopback.
- **Allocation tracking** in realtime paths, and virtual-time drift
  simulations for independent capture and playback clocks.
- **Offscreen rendering** of every screen, including minimum window width.

Design notes for the hardest paths are in
[docs/receive-path.md](docs/receive-path.md),
[docs/send-path.md](docs/send-path.md), and
[docs/session-conductor.md](docs/session-conductor.md). Each records the rules
it was written to keep, and the defects that established them.

## Contributing

Issues, live-session reports, and session logs are welcome and need nothing
signed.

**Code contributions require a signed agreement before they can be merged.**
JamLink has a single copyright holder, which is what keeps a future
dual-licensed edition possible; one contribution merged without an agreement
would remove that permanently. See [CONTRIBUTING.md](docs/CONTRIBUTING.md) and
[CLA.md](docs/CLA.md).

## Licence

JamLink-owned code is [GPL-3.0-or-later](LICENSE). The Windows executable
selects Qt and the Steinberg ASIO SDK under GPL version 3.

Required notices and corresponding-source details are in [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](docs/THIRD_PARTY_LICENSES.md), and
[SOURCE_AND_LICENSES.md](docs/SOURCE_AND_LICENSES.md).
[docs/licensing-options.md](docs/licensing-options.md) records what the current
dependencies do and do not allow.
