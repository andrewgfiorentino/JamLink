# Remote audio receive path

This note covers the design decisions in the receive path that are not obvious
from the code, so the next change does not have to rediscover them.

## Where the playout clock lives

`AudioStreamReceiver::pull` is called from the WASAPI render callback. That
callback is the only clock that determines when a sample actually reaches the
speaker, so it is the clock the jitter buffer runs on. The network worker only
calls `submit`.

Consequences worth keeping:

- There is no second ring between the receiver and the render callback. An
  earlier design drained the receiver into a `SpscAudioRing` on the network
  thread, which added a whole buffer of latency for nothing.
- `pull` always fills its destination, with real audio, concealment, or silence
  before playout starts. Callers never poll for availability, so they cannot
  accidentally consume faster than real time.
- Tests must pace `pull` against a clock. Pulling faster than real time starves
  the buffer permanently and measures nothing. `tests/peer_transport_tests.cpp`
  paces both peers against `steady_clock` for this reason.

The receiver holds no clock of its own; `submit` takes an arrival timestamp from
the caller. That is what makes the impairment tests exactly reproducible.

## Why the target depth is not enough on its own

The adaptive target is computed by the producer from smoothed arrival jitter.
Publishing a larger target does nothing by itself: the amount of audio actually
buffered is set by the difference between the arrival rate and the playout rate,
not by a number in a variable.

`beginPacket` therefore corrects towards the target explicitly:

- below the target, emit one concealed packet **without** consuming a sequence,
  which lets the backlog rebuild (`bufferStretches`);
- above the target plus slack, discard one queued packet (`latencyTrims`).

Both splice the signal and both cross-fade.

The first implementation omitted the stretch, and the buffer stayed at whatever
depth it primed with. Over a virtual hour that produced 20% concealment against
0.5% real loss.

The correction only fires outside a dead zone spanning `target - 1` to
`target + 2`. Correcting on any deviation makes playout hunt: an earlier version
inserted and removed audio about ten times a second, which is audible as warble
even when nothing is being lost. Widening the dead zone cut that tenfold with no
change in the concealment rate.

## Why concealment output never enters the history

The concealment history holds decoded audio only. Synthesised audio is generated
from it but never written back, and `realRunLength_` tracks how many contiguous
real frames sit at the end so the period search can never look across an earlier
splice.

This matters more than it sounds. When synthesised blocks were appended to the
history, an inserted block created a duplicated region, the correlation search
locked onto that insertion rather than the instrument's pitch, and concealment
came out roughly half a period out of phase — 2.8 dB *worse* than zero fill. The
same applies to the recovery cross-fade: it is applied to the output but the
untouched packet is what gets stored, otherwise the fade is re-extrapolated into
a later gap.

## Bounds that exist on purpose

- Depth is capped (32 packets, 160 ms by default). Past that, playing together is
  not realistic and the user should be told rather than buffered. See the
  connection grade in `AppController::connectionQuality`.
- Concealment decays across a burst so a long dropout fades out instead of
  holding a synthetic drone.
- The period search runs once per loss burst, on a 2:1 decimated view with a
  full-rate refinement, so its cost does not scale with the loss rate.

## Telemetry honesty

`packetsConcealed` counts real gaps only. Deliberate stretches are counted
separately as `bufferStretches` so telemetry never overstates network damage.
Round trip and buffer depth are measured; one-way delay is derived from the round
trip and is labelled as an estimate wherever it is shown.
