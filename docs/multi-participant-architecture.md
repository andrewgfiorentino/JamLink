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

**Wired.** The transport now seals a join request with the room key — it has
to, since whoever sends it has not heard from the other end and cannot know
their prefix — and everything after it with the pair key. That is the entire
rekey: one packet type, no negotiation, no extra round trip.

Two properties the wiring had to get right, and does:

- **A forged header cannot tear down a live session.** Keys built from a prefix
  are held aside as candidates and committed only once a packet has actually
  authenticated under them. Otherwise anyone able to send a datagram could
  replace a running session’s keys by putting a different prefix in a header.
- **A peer that restarts still works.** It arrives with a new prefix, the
  candidate path derives fresh keys, and the session re-establishes — which is
  the same path a first connection takes, so it is exercised every session
  rather than only on reconnect.

Verified by disabling derivation and confirming the loopback handshake fails.
A passing suite alone would not have distinguished pair keys working from a
silent fall back to the room key, which is exactly the sort of thing that looks
fine until the second peer arrives.

This changes the key a duo uses, so both people must install it — already true
of every release.

**3. Room membership. — roster done, worker converted, distribution next.** An invite names one host
and the guest connects to it. In a mesh, if Andrew hosts and Mike and Sam each
join him, Mike and Sam are in the same room, have never heard of each other,
and must be connected directly — because that is what a mesh is.

`jamlink::network::RoomRoster` holds who is present, where each of them can be
reached, and answers the question that has no obvious owner: **which end of a
pair reaches out.**

That question is the substance of this step. Every pair has to agree, without
asking each other, which end plays the part the host plays today — the
direction keys and the handshake both depend on it. Two hosts, or two guests,
is a session that never forms and reports nothing while not forming. Both ends
therefore run the same comparison over the same two participant identifiers and
reach opposite answers with nothing exchanged. Identical identifiers are
reported as undecidable rather than guessed, because a copied identity must not
be able to make somebody open a second session against their own name.

Two more rules the roster carries:

- **Reconnecting replaces addresses rather than adding them.** Somebody whose
  router gave them a new port would otherwise leave everyone probing where
  nobody is.
- **Acting on it is idempotent.** The roster arrives on every change and again
  on every reconnect, so a room must not accumulate duplicate sessions with the
  same person.

Introductions come from whoever created the room, since they are the only
participant guaranteed to know everyone. That has a consequence worth stating
rather than discovering: **if the room’s creator leaves, existing pairs keep
playing but nobody new can be introduced.** A duo has the same property today
and nobody notices, because there is nobody left to introduce.

**Half wired.** There is now a `Candidates` packet, and both ends send it once
a session is up. Each end therefore knows where the other can actually be
reached, and the room’s creator accumulates a roster with something real in it
to introduce people with.

Two things it gets right and one thing it does not do:

- **An address list is believed only after it authenticates**, and only when it
  names the participant this session already proved. It is what everyone else
  will be told to probe, so an unverified one would let anybody redirect a room.
- **Both ends report**, which is right rather than incidental: a guest that
  already knows the host’s addresses can re-form a dropped session without
  being handed a fresh invite. The first version of the test asserted
  one-directional reporting, which was a guess rather than a requirement.
- **It does not yet distribute.** The creator knows everyone; nobody is told
  about anybody else.

**The worker is converted.** It was written around exactly one peer: one
handshake, one connected flag, one timeout, one probe schedule, one cipher
pair. All of it is per slot now, and the loop services every slot somebody
holds.

**The session state moved first**, which is the part that could be done
mechanically and proved by the compiler: `connected`, the receive deadline and
the hello and ping clocks were locals in the worker, so deleting them made
anything still expecting them fail to build rather than quietly share one
musician's connection state with another.

**Then the two things that were judgement rather than substitution.**

**Routing.** An arriving packet is attributed to a slot, in this order and for
this reason:

1. the source address against the slots somebody already holds;
2. anything matching nothing that is not a join request: rejected;
3. a join request matching nothing: asks for a slot.

The other way round — deciding from what the packet claims and checking the
address afterwards — would let anyone take a slot by sending anything.

Three things fell out of writing it that were not in the plan:

- **A join request is authenticated before a slot is allocated, not after.**
  Allocating first would let anyone who can reach the port take every seat with
  datagrams full of nothing. Requiring the room secret first costs one
  decryption per join, happens once when a musician arrives, and makes it
  impossible.
- **A name already in the room is routed to the slot that holds it.** A bearer
  invite plus a copied, self-asserted profile ID must not buy a second seat
  beside the person it was copied from. That slot refuses it while its musician
  is healthy and accepts it once they have timed out, which is exactly the
  existing rule rather than a new one.
- **This machine's own join request, handed back by a hairpinning router,
  authenticates perfectly** — it was sealed with the room key this end holds,
  so nothing in the payload can catch it. It is recognised by its nonce prefix,
  which is drawn per slot from the system generator and is genuinely ours. Not
  by the identity in it: two people running an unconfigured build legitimately
  share one, and refusing on that basis refused every real peer. That was found
  by the suite going red, which is the argument for having converted a working
  transport rather than writing a new one.

**Fan-out and lifecycle.** The loop services each live slot, opens one when the
roster says to, and lets one peer time out without disturbing the others.

The hazard the plan named — that the send pacer releases one packet on a
schedule, and draining it per peer gives the first musician everything and
everybody after them silence — is real, and is now the thing a test
deliberately reproduces. Capture, limiting and encoding happen once, outside
the per-peer loop; only the sealing is per recipient, which is what a mesh
actually requires. Reintroducing the fault fails exactly one assertion, and the
two-person suite stays green throughout it. That is the check that distinguishes
a working fan-out from a lucky one.

### What was decided, and what it cost

**Attributing a packet.** As above. A slot still finding a path accepts a
`HelloAck` or a refusal from an address it never probed, because a router may
rewrite the source and several candidates are in flight; everything after the
handshake is held to the settled endpoint.

**Allocating a slot.** First free one, then the one belonging to whoever has
been gone longest, then refused. The capacity guard answers whether there is
room rather than a second rule being invented at the door — so measured strain
still outranks arithmetic, and six is still the ceiling.

**Telling a refused musician.** A new `RoomFull` packet, sealed with the room
key exactly like the join request it answers, because admission happens before
a pair exists and there is no pair key to use. That puts it on the same trust
boundary as the `Hello`: anyone who could forge one already holds the room
secret and is already in the room. It carries the transport's own nonce prefix
and counter rather than borrowing a slot's, since it is sent precisely when no
slot was available. The musician sees "this room is full" — not a network
error, because nothing failed and there is nothing to retry.

**What a room being connected means. Decided: at least one.** `state ==
Connected` means at least one other musician is in session, which is the answer
to "can I play at all". A room where one of three has dropped is still a room
the other two can play in, and calling it disconnected would stop a session
that is working. Whether everybody the roster names is here is the other honest
answer, and it is `roomComplete`, reported separately. The conductor, the room
screen and the recording gate all read the first; the room screen reads the
second as well, so it can say a room is still filling up. Named once in
`PeerTransportTelemetry` so the three cannot answer it differently.

**When a peer times out.** The slot and its addresses are kept and marked not
connected, because they are what let a dropped session re-form without a fresh
invite. Released only when another musician actually needs one.

**Sending to several peers.** As above. The recipient list is taken before the
release loop begins, so a peer arriving part way through a pass starts at the
next packet rather than at half of this one.

**One encoder, so one rate — the lowest anybody asked for.** Not in the plan,
and it has to be decided because the bitrate controllers are per peer while the
encoder is shared. Encoding separately per peer to spare the healthy links a
smaller packet would multiply the most expensive thing the send path does by
the size of the room; taking the lowest means a musician whose link cannot
carry 96 kbit/s is still sent to, which the alternative does not.

**Chat is room-wide, so it fans out — and that is not one line.** A message is
delivered to everybody who was in the room when it was first sent, and is not
finished until each of them has acknowledged it; erasing it on the first
acknowledgement would drop it for every musician after the first. Somebody who
joins afterwards does not get the backlog. Somebody who leaves before
acknowledging is not waited for, because reporting a message undelivered
because one of three walked away describes the wrong thing to the two who
received it.

Inbound de-duplication had a defect that only exists in a room: every musician
numbers their own messages from one, so an ID alone collides across senders and
the collision silently drops the second person's message as a duplicate of the
first. It is keyed by sender and ID now.

**Telemetry became per peer.** The plan called aggregation "cheaper and worse",
and it is: a bundle that cannot say which musician the trouble was with is the
only thing anybody reads one for. Round trip, buffer depth, jitter and
concealment are per link, and the flat fields stay the first connected peer's,
which is what every existing reader already meant by them.

**Mixing.** Playback mixes every peer's receivers. Each musician's level, mute
and meter apply to their own contribution before it is added — metering the mix
instead would leave one bad connection showing on everybody's meter with
nowhere to turn it down. The controls still mean "everyone", because the
interface has one level per stream rather than one per musician; that is step
4, and it is what turns them from a convenience into how somebody copes with
one person's bad connection.

### What this does not yet do

**Nothing distributes the roster.** The room's creator accumulates one and can
introduce people with it; nobody is told about anybody else. The worker will
open a session with anybody the roster names and it has not reached — that path
is written and asks the roster which end reaches out — but until distribution
lands, the roster only ever names people this end is already connected to, so
it finds nobody.

The practical consequence: **three musicians can play together today only in a
room where everybody joined the same host.** That works, and is what the tests
exercise: the host holds a slot per guest, sends to each of them separately,
and one leaving does not disturb the rest. What it is not yet is a mesh — the
guests cannot hear each other. Distribution is the next piece of work, and it
is small compared to this one precisely because everything it needs to drive is
now in place.

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
