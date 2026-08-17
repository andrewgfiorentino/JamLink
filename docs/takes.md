<!-- Copyright (c) 2026 Andrew Fiorentino -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Takes

A playable WAV proves that samples reached a disk. It does not prove that the
recording finished.

An interrupted take leaves files that open perfectly and are missing their last
ten seconds, and nothing in them says so. That is the problem this exists to
solve: not storage, which the recorder already handles well, but **truthfulness
about what was captured**.

## What was kept

`SessionRecorder` is untouched. Its preallocated realtime handoff, its writer
thread, its aligned tracks, its dropped-frame accounting and its crash-tolerant
header updates are all valuable and all still there. Nothing about the realtime
path changed to make room for this.

The manifest is written *alongside* the audio, from the control thread, never
from the audio callback.

## Only finalisation reaches Ready

```
Preparing -> Recording -> Stopping -> Finalizing -> Ready
                 |
                 +-> (process dies) -> RecoveredNeedsReview
```

`TakeJournal::begin` writes a manifest that says the take is in progress. From
that moment until `finalise` succeeds, the state on disk is the truth: if
JamLink stops, restarts, and finds a manifest that never reached `Ready`, the
take was interrupted.

Finalisation is the only path to `Ready`, and only when its write succeeds. A
failed finalisation leaves the take discoverable rather than silently complete.

## Complete and flawless are different claims

A take that finished cleanly may still be missing frames the writer could not
keep up with, or contain concealed audio that arrived damaged. `droppedFrames`
and per-source `knownGapFrames` record that, and `hasKnownGaps()` is
deliberately separate from `complete()`.

A take can be finished and imperfect. Conflating the two would make the archive
misleading in exactly the case a musician most needs it not to be.

## Atomic replacement

Each write goes to `take.jamlink.new` and is renamed over `take.jamlink`. A
process killed mid-write therefore leaves either the previous manifest or the
new one, never a half-written file, which would make a real recording
unreadable and is worse than a stale manifest.

A leftover `.new` from an interrupted write is ignored, not mistaken for the
manifest.

## Source identity

Sources carry more than a filename, because the same logical source appears in
several forms over a take's life: captured locally, received over the network,
and later possibly replaced by the other side's pristine original. All three are
the same musician playing the same instrument.

```
sourceId        stable within the take
participantId   whose audio this is
role            instrument or voice
origin          local-capture or network-received
knownGapFrames  what is missing, if anything
sha256          empty until verified
```

`origin` is what will let a later release repair a take: a network-received
source damaged by a dropout can be replaced by the sender's local-capture
original, and the manifest already knows which is which.

## Recovery never deletes

Anything found mid-recording at startup is kept exactly as it is, marked
`RecoveredNeedsReview`, and given a reason. Once reviewed it stops being
reported, so a musician is not asked about the same take at every launch.

## Forward compatibility fails safe

A state written by a newer JamLink that this build has never heard of parses as
`RecoveredNeedsReview`, not as `Ready`. A reader that does not understand what
it is looking at must not call a recording clean.
