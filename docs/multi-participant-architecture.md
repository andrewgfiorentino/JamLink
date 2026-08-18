<!-- Copyright (c) 2026 Andrew Fiorentino -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# More than two musicians

JamLink is a duo today: one remote address, one pair of keys, one participant,
one set of receive buffers. The goal is a small group — guitar, bass, keys,
drums, voices — playing together.

This records the decision, the arithmetic behind it, and the order the work has
to happen in, so that none of it has to be re-derived later.

## The decision: full mesh

Everyone sends their own audio directly to everyone else.

**Why.** No hub, so nobody's machine is carrying the room and nobody's upload
becomes everybody's ceiling. No extra hop, so the delay between any two players
is the delay of the path between them and nothing more — which is the entire
point of the project. One person's connection failing takes out the paths to
that person rather than the session.

**What was rejected.** A hub keeps upstream flat regardless of group size,
which is genuinely better on thin connections. It was not chosen because it
makes one participant special, adds a hop to every path that does not go
through that participant's own ears, and turns one household's evening
bandwidth into the whole room's limit.

**The cost, stated plainly.** Upstream scales with the number of *other*
people. That is not a detail to discover in the field:

| Musicians | Streams sent from each machine | Upstream from each machine |
| --------- | ------------------------------ | -------------------------- |
| 2         | 2                              | about 0.32 Mbit/s          |
| 3         | 4                              | about 0.64 Mbit/s          |
| 4         | 6                              | about 0.96 Mbit/s          |
| 5         | 8                              | about 1.3 Mbit/s           |
| 6         | 10                             | about 1.6 Mbit/s           |

At the current 96 kbit/s per stream and two streams each. The bitrate
controller can lower that when a link says it cannot carry it, which buys real
headroom — a room at 32 kbit/s per stream costs a third as much.

## What that forces, and what is already done

**A capacity guard, before anything else.** The failure mode of a mesh is that
one more person joins and *everybody* gets worse at once, with nothing on
screen connecting the two. Somebody who added a fifth musician would conclude
JamLink is unreliable, and they would be describing the arithmetic rather than
a defect.

`jamlink::control::RoomCapacity` decides this and is wired into admission
already, at two people, so it is exercised from the day it exists rather than
appearing untested alongside the transport work. Its rules:

- **Measured strain outranks arithmetic.** If the send rate has already had to
  be reduced for the people who are here, the connection has answered the
  question. A generous number typed into a settings box does not make it
  faster.
- **An unknown uplink never blocks anyone.** Almost nobody knows their upload
  speed, and what an operator advertises is not what a house gets at nine in the
  evening. Unverified is the honest verdict and it admits people.
- **Six is the ceiling regardless of bandwidth.** Paths grow with the square of
  the group, and so do the ways for one of them to be the problem.
- **A lowered send rate makes more room rather than less**, so this and the
  bitrate controller cannot contradict each other.

## The order the rest has to happen in

Each of these is a real piece of work, and the order is not arbitrary — every
step depends on the one above it.

**1. A peer becomes a slot rather than the peer. — done.** Every piece of
per-peer state in the Windows transport was a single object: the remote
address, the replay window, the nonce prefix and counter, the send sequences,
the participant identity, the receive buffers, the decoders, the remote gains
and meters, the per-peer bitrate controllers, and the candidate negotiation.
They now live in a `PeerSlot`, held as an array of which only slot zero is
created, and every use goes through `peer()`.

The old members were **deleted** rather than left alongside the new ones, so a
missed conversion is a compile error rather than a second copy of the truth
that silently disagrees with the first. Eighty-eight call sites moved; the wire
format did not change and neither did any test.

What deliberately stayed shared: the socket, the capture rings, the send
pacers, the send limiters and the local mute state all belong to this machine
rather than to any one peer. A mesh encodes a packet once and seals it
separately per recipient, so everything on the near side of the encoder is
common. The room secret stayed shared too, because per-peer key material is
step 2 and has to be — reusing a nonce across two peers under one key would be
a serious defect rather than an optimisation.

**2. Encode once, encrypt per peer. — key schedule done, wiring next.**

Starting this turned up something worth writing down before any of it is
wired. Keying both directions from the room secret alone, which is what happens
today, is sound with one peer and is two separate defects with several:

- **Everyone in the room can read everyone else’s traffic.** Two musicians’
  chat and audio would be decryptable by a third who was merely in the same
  room. That is not what a private session means.
- **A packet is not bound to its recipient.** Sealed under a key everybody
  holds, a packet addressed to one musician authenticates perfectly as one
  addressed to another. Cross-peer confusion of that kind is a security defect
  rather than a glitch, and it is invisible while there is only one peer to be
  confused with.

The obvious fix — renegotiate a key per pair — costs a round trip and a rekey
point, both of which are risk in the handshake. It is not needed. Every packet
already carries the sender’s eight-byte nonce prefix in the clear, because the
receiver needs it to reconstruct the nonce. Both ends therefore hold both
prefixes from the first packet onward. Sorting the two makes the pair identity
symmetric, so each side computes the same discriminator without agreeing who is
“first”, and direction is layered on top exactly as now so a packet still
cannot be reflected back at its sender.

No new field, no extra round trip, and every pair keyed apart.

`jamlink::network::PeerKeySchedule` implements and tests that derivation. It
fails closed when no HMAC is installed, because sending in the clear following
a derivation failure would be the worst available recovery.

What remains is the transport wiring: creating the ciphers once the remote
prefix is known rather than at worker start, and re-deriving when a peer
restarts and its prefix changes. That changes the key a duo uses, so it is a
release both people must install — which is already true of every release.

**3. Room membership.** Today an invite names one host and the guest connects
to it. With a mesh, a third musician has to end up connected to *both* of the
first two, which means the room has to tell arriving members about each other.
The signalling service already models a session as a set of participants with a
bounded roster, which is the shape this needs; it is not deployed, so the first
version can distribute membership over the existing authenticated control
channel from whoever created the room.

**4. Mixing, and the interface.** Each remote participant needs its own level,
mute, meter, and jitter buffer — the per-stream controls that exist for one
friend, per person. The interface is already scaffolded for this: the room page
renders a participant list and there is a four-musician offscreen fixture.

**5. Recording.** A take currently has four live tracks and two originals.
With N participants it has 2N live tracks. The per-source exclusion and the
take manifest already describe sources by participant, so this is mostly a
matter of the recorder growing beyond a fixed six.

## What is deliberately not planned yet

**Relay.** Still nothing. A mesh makes the traversal problem worse, not better:
every pair of musicians needs a path, so one participant behind a symmetric
router breaks more than one connection. The rendezvous service is written and
tested but not deployed, and deploying it needs an account that cannot be
created automatically.

**Stereo.** Keys and drums want it, and it multiplies every figure in the table
above by two. It should land after the mesh rather than during it, so that the
bandwidth arithmetic changes once and is re-checked once.
