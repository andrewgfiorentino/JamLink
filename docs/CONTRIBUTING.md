<!-- Copyright (c) 2026 Andrew Fiorentino -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Contributing to JamLink

Thank you for wanting to help. Please read the licensing section before opening
a pull request — it is short, and it is the one thing that cannot be sorted out
afterwards.

## Contributor Licence Agreement

**Every code contribution requires a signed Contributor Licence Agreement
before it can be merged.** See [CLA.md](CLA.md).

This is not bureaucracy for its own sake. JamLink is currently GPL-3.0-or-later
and has a single copyright holder, which is what keeps a future dual-licence or
commercial edition possible at all. A single merged contribution without a CLA
would permanently remove that option, because relicensing then needs the
agreement of every contributor, forever, including anyone who has stopped
answering email.

The CLA does not take your copyright away. You keep it, and you grant a licence
broad enough that the project can be relicensed later.

Contributions that need no CLA: issues, bug reports, test results from a live
session, documentation typo fixes, and translations of user-facing text.

## Before opening a pull request

- Both configurations must build clean. Warnings are errors here.
  - `cmake --build --preset windows-debug` then `ctest --preset windows-debug`
  - `cmake --build --preset windows-gui-debug` then `ctest --preset windows-gui-debug`
- New behaviour needs a test that fails without the change. If the behaviour
  cannot be tested, say so in the pull request and explain why.
- Realtime rules are absolute in the audio callback: no allocation, no file or
  socket access, no locks, no logging. `AudioStreamReceiver` and
  `OutgoingAudioPacer` are the models to follow.
- Do not report an estimate as a measurement. If a number was inferred rather
  than measured, the text a musician reads has to say so.
- No control may be shown that cannot act. A switch that moves and changes
  nothing is treated as a defect, not a placeholder.

## Third-party code

Do not add a dependency without raising it first. Two constraints apply:

- Anything incompatible with GPL-3.0-or-later cannot be added at all.
- Anything that would be awkward under a future commercial licence should be
  avoided, or kept isolated behind an interface the way the Steinberg ASIO SDK
  is. `jamlink_asio_isolation_tests` enforces that particular boundary and will
  fail the build if it erodes.

Current third-party licences are recorded in
[THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
