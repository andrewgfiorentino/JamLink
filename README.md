# JamLink

**Play music together over the internet, in real time.** JamLink is a free
Windows app for small private sessions — you and a few friends, playing at the
same time, not trading recordings back and forth. There is no account to make,
nothing to subscribe to, and no company server your music passes through: the
audio goes straight between your computers.

> Status: **0.4.8-test**. It works and it has been used for real sessions
> between two houses, but it is not finished. Expect rough edges.

## What it feels like to use

**Being early is the whole point.** With a proper audio interface, you hear
yourself about five milliseconds after you play — close enough that playing
together feels like being in a room rather than sending messages. It also works
with plain Windows audio, just not as fast.

- **Up to six people, everyone hearing everyone.** Each person gets their own
  volume, mute and meter, so you can turn one friend's guitar down without
  touching their voice. If somebody drops out, the rest of you keep playing.
- **One code to connect.** Whoever starts the room gets an invite to send.
  Nothing else is exchanged, and nobody needs to sign up. If you and a friend
  are in the same building, JamLink notices and keeps the audio on your local
  network instead of sending it out to the internet and back.
- **Guitar and voice stay separate.** Two independent channels for each person,
  each with its own level and mute. You can mute your microphone to the room
  while your guitar keeps playing — separately from whether you hear yourself.
- **A mute you cannot forget.** One press stops sending everything, and while
  it is on the room says so plainly instead of leaving you wondering whether
  anyone can hear you.
- **It copes with a rough connection.** JamLink keeps a small cushion of audio
  and grows or shrinks it as your connection changes. When a piece of sound
  goes missing on the way, it carries on the note you were already playing
  rather than dropping to silence — so a hiccup sounds like a smudge instead of
  a hole. If your upload genuinely cannot keep up, it quietly sends a little
  less rather than breaking up, and goes back to full quality once things
  settle.
- **Nobody can listen in.** Everything is encrypted between each pair of
  players, so even someone else in the same room cannot read your audio or your
  messages. Nobody can join without your invite, and audio captured off the
  wire cannot be recorded and pushed back at either of you later.
- **A tuner** that mutes your guitar to the room while you use it, so nobody
  has to listen to you tune. Leave that off and you can keep it open beside the
  session.
- **Recording that keeps an untouched copy.** Every take records each person's
  guitar and voice as separate files. Alongside those, JamLink keeps a copy of
  *your own* playing taken before it was mixed for your headphones and before
  the internet could touch it. The first set is what everyone heard; the second
  is what you actually played, and a dropout cannot damage it.
- **Record only what you meant to.** Any of the channels can be left out of a
  take, and one you turn off is never written to disk at all. The take notes
  what you left out on purpose, so a missing file is never mistaken for one
  that failed.
- **Takes that open.** Each finished take writes a session file next to the
  audio, so it opens in Reaper with every part already lined up on one
  timeline, named by who played it. No dragging six files into place and hoping.
- **Takes that admit what they are.** If a recording was interrupted, or the
  disk fell behind and left gaps, the take says so. It is never presented as
  clean when it is not.
- **Private text chat** on the same encrypted connection.
- **One clear answer, not a dashboard.** Rather than a wall of green and amber
  lights, JamLink works out whether you can actually play and tells you. When
  something needs fixing it offers one thing to do. And it only claims you are
  ready when sound is genuinely moving — never because a connection merely
  exists.
- **It does not overstate what it knows.** Where a number is an estimate rather
  than something measured, it says so.

## Installing

Download the latest ZIP from [Releases](../../releases), unzip it somewhere
normal, and run `JamLink.exe`.

**Everyone in the session needs the same version.** JamLink checks, and refuses
to connect mismatched versions rather than half-working in a way that is hard
to diagnose.

The app is not code-signed yet, so Windows will warn you about it. Compare the
published SHA-256 with the `.sha256` file if you want to be sure, then choose
**More info → Run anyway**.

You need Windows 11 (64-bit) and **headphones**. There is deliberately no echo
cancellation — it would add delay and change how your instrument sounds.

[docs/running-a-session.md](docs/running-a-session.md) walks through setting up
a session.

## Setting up your sound

**If you have an audio interface, choose its ASIO driver** for both input and
output. ASIO runs recording and playback together off one clock, which is what
makes it fast. Plain Windows audio runs them separately with a conversion step
in between, which costs both time and quality.

**Leave buffer size on Auto.** JamLink opens your interface at the size its own
driver recommends — the same number your interface's control panel uses — and
only moves up if your computer actually starts dropping audio. The smallest
size a driver will accept is not a sensible place to run: some will accept a
third of a millisecond, where a perfectly healthy computer sitting idle will
stutter.

**Some combinations simply cannot work.** An interface's ASIO driver takes over
the whole device, so you cannot use its input while sending your headphones
through Windows. JamLink spots this as you choose it, tells you which single
device to change, and gives you a button that changes it.

**If your interface has its own direct monitoring**, use that and turn
JamLink's monitoring off for that input. Everything else — meters, the tuner,
recording, and what your friends hear — carries on as normal.

## What it cannot do yet

- **Windows only**, 64-bit.
- **Six people is the limit**, and the practical limit may be lower. Everyone
  sends their audio to everyone else, so each extra person costs *everybody*
  more upload, not just the person joining — roughly another 0.3 Mbit/s each.
  JamLink will decline to let someone in when your connection has already shown
  it cannot carry the people who are here.
- **Some connections cannot host.** If your internet connection changes the
  address JamLink is reached on every time it is used, an invite you create
  leads nowhere. JamLink detects this and tells you to have your friend create
  the invite instead — the session is identical either way. If *everybody* is
  behind that kind of connection, a direct session may not be possible at all;
  there is no fallback server yet.
- **You cannot send your untouched recordings to each other.** Each person
  keeps a clean copy of their own playing, but it stays on their machine, so
  your copy of a friend's part is still whatever arrived over the internet.
- **The metronome is not finished.** The click and count-in exist inside the
  app but there is no control for them yet.
- **Everyone needs the same version**, and that has mattered recently: the way
  sessions connect changed in 0.4.1, invites changed in 0.4.3, and encryption
  changed in 0.4.4 — so anything older than 0.4.4 cannot talk to anything newer
  at all.

---

## For developers

### Building

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

### How it is tested

When realtime audio goes wrong it is very hard to tell whether the fault is the
software, the computer, or the internet connection — and guessing wrong wastes
everybody's evening. The tests exist to make those three impossible to confuse.

- **A fake bad connection.** Audio is pushed through simulated loss, delay,
  reordering and duplication, and the result is compared against the original
  signal rather than listened to and judged by ear.
- **Nothing goes missing.** Every piece of captured audio must be accounted for
  — sent, still waiting, or deliberately discarded and counted — so sound
  cannot quietly vanish between your instrument and the network.
- **Real connections.** Up to six copies of JamLink connect to each other over
  real network sockets and exchange real encrypted audio, including reconnects,
  and including deliberately malformed and hostile traffic.
- **No hidden pauses.** The parts that run while audio is playing are checked
  to make sure they never stop to allocate memory, which is what causes clicks.
  Clock drift between separate recording and playback devices is simulated
  rather than waited for.
- **Every screen is drawn and checked**, including at the narrowest window size
  the app allows.

Design notes for the hardest parts are in
[docs/receive-path.md](docs/receive-path.md),
[docs/send-path.md](docs/send-path.md), and
[docs/session-conductor.md](docs/session-conductor.md). Each records the rules
it was written to keep, and the bugs that established them.

### Contributing

Bug reports, session reports and logs are very welcome and need nothing signed.

**Code contributions need a signed agreement before they can be merged.**
JamLink has a single copyright holder, which is what keeps future licensing
options open; one contribution merged without an agreement would close them
permanently. See [CONTRIBUTING.md](docs/CONTRIBUTING.md) and
[CLA.md](docs/CLA.md).

### Licence

JamLink-owned code is [GPL-3.0-or-later](LICENSE). The Windows executable
selects Qt and the Steinberg ASIO SDK under GPL version 3.

Required notices and corresponding-source details are in [NOTICE](NOTICE),
[THIRD_PARTY_LICENSES.md](docs/THIRD_PARTY_LICENSES.md), and
[SOURCE_AND_LICENSES.md](docs/SOURCE_AND_LICENSES.md).
[docs/licensing-options.md](docs/licensing-options.md) records what the current
dependencies do and do not allow.
