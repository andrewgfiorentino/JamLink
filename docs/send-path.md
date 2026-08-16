<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The send path

The receive path has [its own note](receive-path.md). This one covers the other
direction: how audio captured on this machine reaches the other person.

```
capture callback -> SPSC ring -> OutgoingAudioPacer -> AES-256-GCM -> UDP
                    (thread       (rate conversion
                     boundary)     and schedule)
```

## Three rules

These are written down because each was broken in a shipped build, and in every
case the symptom looked like a bad network rather than a defect in JamLink.

### 1. Outgoing audio is captured audio

The stream sent to the other person is taken in the capture callback, at the
capture device's own sample rate. It is never taken from the playback callback.

Feeding the network from the playback side makes what your friend hears a
function of *your* playback device. On Windows shared-mode audio the capture and
render endpoints run on independent clocks with a converter bridging them, and
that converter zero-fills whatever it cannot supply. Because its return value
was discarded, every underrun placed a block of digital silence on the wire as
though it were the guitar. A tester heard this as bit-crushing and glitching
across a link whose round trip was 4 ms.

ASIO was unaffected, because capture and playback there share one clock in one
callback — which is why the same session sounded fine in one direction and
broken in the other, and why the fault was initially misread as packet loss.

### 2. Packets leave on the cadence they represent

A receiver measures the spacing of arrivals and cannot distinguish a sender's
bursts from network jitter. Releasing a backlog back to back therefore makes a
flawless link measure as an unstable one: the first successful two-home session
ran over a 4 ms round trip and still reported a 135 ms receive buffer, with
concealment running almost continuously.

`OutgoingAudioPacer` advances its deadline by exactly one packet per packet
sent, so the schedule stays tied to the media clock rather than to whenever the
sender happened to wake up.

### 3. Captured audio is never stranded, and a drop is never silent

The design that first removed the bursts capped catch-up at four packets per
wake-up and then, if still behind, rebased the schedule forward to the current
time. Rebasing abandons the deficit: the audio behind it stayed in the
converter, the backlog grew on every late wake-up, and once the converter filled
it discarded in chunks. That is a steady leak that reads as loss on the far end.

So: lateness is made up by sending sooner, never by moving the deadline. The
only audio deliberately discarded is a backlog older than a live session can
use, and `OutgoingAudioPacer::Telemetry::framesDiscarded` counts every frame of
it. A related trap sits inside the converter — asking it for more than it holds
fills what it can, marks it unprimed, and loses the remainder — so a read is
never attempted unless a whole packet is available.

Backlog still standing after everything due has gone out is drained gradually
rather than carried, because nothing downstream will ever remove it; a sender
blocked for half a second would otherwise leave both players permanently that
much further apart. This is judged once per wake-up rather than between packets,
since a sender that wakes every 50 ms legitimately holds 50 ms of capture in the
moment before it sends.

## What the tests hold to

`tests/outgoing_audio_pacer_tests.cpp` drives the pacer through virtual time
with audio produced at exactly the device rate. The load-bearing assertion is
conservation: every frame accepted was either sent, is still queued, or was
counted as discarded. Stranding cannot pass it.

The rest cover the media rate under wake-up periods from 5 ms to 50 ms — the
15.6 ms case is the Windows system tick, which is what a 5 ms `select` timeout
actually delivers — a capture rate that differs from the network rate, recovery
after a stall, and a constant input arriving as a constant, which is what makes
manufactured silence fail the build.
