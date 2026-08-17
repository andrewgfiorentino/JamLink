<!-- Copyright (c) 2026 Andrew Fiorentino -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Keeping the licensing options open

None of this is legal advice. It is a record of what the repository actually
contains, so that a decision about commercialising JamLink can be made from
facts rather than from memory, and taken to a lawyer without having to rebuild
the picture first.

## Where JamLink stands today

JamLink is released under GPL-3.0-or-later, and **has a single copyright
holder**. That is the whole basis of every option below: a sole holder may
relicense their own work at any time, to anything, for any future version. GPL
is a licence granted outward, not a constraint on the person granting it.

Two facts follow, and both matter.

**Every release already published stays GPL forever.** Whoever received 0.1.0
through 0.4.0 keeps the right to use, modify, redistribute and fork that code
permanently. It cannot be withdrawn. What can change is what is granted from
here on.

**One merged contribution without a CLA ends it.** Relicensing would then need
the individual agreement of every contributor, in perpetuity, including anyone
who stops replying. This is why [CONTRIBUTING.md](CONTRIBUTING.md) and
[CLA.md](CLA.md) exist and why the pull request template leads with them.

## What the dependencies allow

### Steinberg ASIO SDK — the gate

`third_party/asio-sdk/LICENSE.txt` offers exactly two paths: GPL version 3, or
a proprietary Steinberg licence. JamLink currently takes the GPL option.

The proprietary path states plainly that *before publishing software under it,
a copy of the License Agreement signed by Steinberg Media Technologies GmbH
must be obtained*. Under that agreement the SDK sources may not be
redistributed, which the current corresponding-source archive does.

ASIO is the entire low-latency story on Windows, so there is no closed-source
commercial JamLink without that signature.

**This is the long pole and it runs on Steinberg's timetable.** It cannot be
automated, because it needs a legal identity and a countersignature.

- Agreement and developer registration: <https://www.steinberg.net/developers/>
- Free of charge, but an application and an approval
- Worth starting well before it is needed, and there is no cost to holding one

### Qt — already in the right shape

The desktop links `Core`, `Gui`, `Network`, `Qml`, and `Quick`. Every one of
those is available under **LGPLv3**; none are the GPL-only modules such as
Charts, Data Visualization, or Virtual Keyboard.

Packaging deploys Qt as DLLs through `windeployqt`, which is dynamic linking —
the arrangement LGPL is written for. A closed-source JamLink is therefore
possible under LGPL, provided the usual obligations are met: ship the LGPL
text, state that Qt is used, keep the libraries replaceable, and do not link
statically. A Qt commercial licence is the alternative and is what static
linking would require.

The current README records Qt as taken under GPL version 3. That is an
election, not a constraint, and it would be re-recorded under a different
choice.

### Material Design Icons — no constraint

Apache License 2.0. Permissive, fine under a proprietary licence, and needs
only its attribution preserved.

## Three paths

**Dual-licence.** GPLv3 stays public; commercial licences are sold to anyone who
cannot accept it. Possible only while there is a sole copyright holder.

**Proprietary from here.** Needs the Steinberg agreement, Qt under LGPL or
commercial, and relicensing of JamLink's own code. Earlier releases stay GPL.

**Stay GPL, sell the service.** GPL has never prevented charging money. Hosted
rendezvous, a relay, accounts, support. No Steinberg negotiation, no
relicensing, no CLA friction — and the relay planned for a later release is
naturally a hosted service.

Given where JamLink is — pre-acceptance-test, two users, no revenue — the third
costs least and forecloses nothing, and the first stays available precisely
because authorship is undivided.

## What is enforced automatically

- Copyright notices on every JamLink-owned source file, so ownership is
  asserted in the code and not only in a licence file.
- `jamlink_asio_isolation_tests` fails the build if the ASIO SDK is included
  anywhere but `src/platform/windows/asio_soundcheck_audio_service.cpp`. That
  boundary is what keeps a licence change from becoming a rewrite. The test has
  been verified to fail when the boundary is crossed, not merely to pass today.
- The pull request template requires a CLA line before a contribution can be
  merged.

## What still needs a person

- Applying to Steinberg. Needs a legal identity and a signature.
- Choosing among the three paths.
- A lawyer's review before anything is sold, and before this CLA is relied on
  for a contribution that matters.
