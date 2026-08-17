<!-- Copyright (c) 2026 Andrew Fiorentino -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# The session conductor

Every subsystem in JamLink already knows something true. The audio service
knows whether a device opened. The transport knows whether a socket is bound
and a peer authenticated. The receive path knows whether packets are arriving.

None of them knows whether two people can play.

Asking the interface to work that out from a dozen unrelated states is how a
room ends up reporting "connected" while nobody can hear anything — which is
exactly what happened in field testing, more than once, and cost several
sessions to diagnose.

The conductor is that missing layer: subsystem truth in, one musician-meaningful
answer out.

## Why it is a pure function

It holds no clock, no threads, and no I/O. `update(evidence, now)` is a pure
function of the facts it is given, which is what makes every transition below
reproducible in a test rather than only observable during a live session.

Timestamps are supplied by the caller for the same reason the receive buffer
takes its arrival times that way.

## The rule the design serves

**A phase that says someone can play is gated on evidence that audio is moving,
never on a socket existing.**

`peerAuthenticated` proves two programs agree with each other. It does not prove
two people can hear each other. `ReadyToPlay` therefore also requires
`mediaProgressing`, and `an_authenticated_peer_is_not_by_itself_ready_to_play`
fails the build if that stops being true.

The same rule applies throughout: a recording is not complete because a file
handle exists, and a device opening is not proof the route is usable.

## Order of judgement

The order of the checks is the design, not an accident of writing them down.

1. **A take being written outranks everything.** Losing a recording to a tidy
   shutdown is worse than any delay, so finalising wins even over a connection
   that has fallen over.
2. **A build mismatch is the only final failure.** Nothing about waiting or
   retrying can fix it.
3. **Audio before network.** Being reachable is worth nothing without something
   to play into, so a musician is never sent to fix a router while their
   interface is missing.
4. **Media before comfort.** Whether audio is arriving is decided before whether
   it is arriving well.
5. **Degraded is not broken.** A struggling connection keeps `playable` set,
   because a musician who can still play should be told to keep playing.

## One thing at a time

`GuidanceAction` is a single value, never a list. A clipping input matters, but
not while the session cannot start at all, so it speaks only once nothing more
important is wrong — `only_one_thing_is_asked_of_the_musician_at_a_time` holds
that.

Guidance also distinguishes *what the musician must do* from *what JamLink is
already doing about it*: `RecoveryPosture` says whether to act or simply wait.

## Lifecycle history

Transitions are recorded semantically — `Reconnecting -> ReadyToPlay` — with a
timestamp and nothing else. No invite, no key, no endpoint, no chat. The history
is bounded at 64 entries and drops the oldest, because what a session is doing
now is more useful for support than how it began.

This is the intended source for the lifecycle section of a support bundle.

## What it deliberately does not do

It does not replace the subsystem state machines. `PeerConnectionState`,
`SoundcheckAudioState` and the readiness tracker remain authoritative about
their own domains; the conductor reads them and answers a different question.
