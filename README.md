# JamLink

**Play music together over the internet, in real time.** JamLink is a free and
open-source Windows application for private two-person sessions: guitar and
voice travel as separate encrypted streams over a direct connection, with no
server in the middle and no account to create.

> Status: **0.4.0-test**. Under active development and validated in two-home
> field testing, but not yet at a 1.0 release. Expect rough edges.

## What it does

**Low latency is the whole point.** With an ASIO interface, monitoring runs at
around 5 ms and playing together feels like being in a room rather than
trading messages. Windows shared audio works too, at a higher floor Windows
imposes.

- **Direct encrypted audio.** AES-256-GCM with per-direction keys, a replay
  window, and authenticated participant identity. Audio goes peer to peer.
- **One code to connect.** The host creates an invite and sends it; nothing
  else is exchanged. Port mapping is attempted automatically over UPnP, PCP,
  and NAT-PMP, and Windows Firewall is detected and repairable from inside the
  application.
- **Guitar and voice as separate streams.** Each has its own jitter buffer,
  level, and mute, so a friend's guitar can be turned down without turning
  their voice down.
- **Survives an imperfect connection.** An adaptive jitter buffer sized from
  measured arrival jitter, and pitch-synchronous packet-loss concealment that
  extrapolates the instrument's own waveform rather than inserting silence.
- **A chromatic tuner** that mutes your guitar to the room while you use it,
  and can stay open beside the session once you turn that off.
- **Four-track recording.** Your instrument, your voice, and each of your
  friend's streams, written as separate WAV files aligned to one timeline.
- **Private text chat** over the same authenticated connection.
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

If your interface does its own zero-latency monitoring, turn JamLink's monitor
off for that input and use the hardware path. Capture, meters, clipping
detection, the tuner, recording, and what your friend hears all continue.

## Known limitations

- **Windows only**, x64.
- **Two people per room.** Larger sessions are not implemented.
- **No relay.** If both ends are behind carrier-grade or symmetric NAT, a
  direct connection may be impossible. JamLink says so rather than pretending,
  and will suggest the other person host if they can.
- **Uncompressed audio.** 48 kHz mono PCM per stream, roughly 1.6 Mbit/s
  upstream. There is no codec yet.
- **Recording captures what arrived**, concealment included. Exchanging clean
  local masters after a take is not implemented.

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

Design notes for the two hardest paths are in
[docs/receive-path.md](docs/receive-path.md) and
[docs/send-path.md](docs/send-path.md). Both record the rules each was written
to keep, and the defects that established them.

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
