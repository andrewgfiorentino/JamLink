# Vendored libopus

Upstream: <https://github.com/xiph/opus>
Version: **1.4** (tag `v1.4`, commit `82ac57d9f1aaf575800cf17373348e45b7ce6c0d`)

Unmodified upstream sources. Licence text is in `COPYING`, and
`LICENSE_PLEASE_READ.txt` carries the patent position.

## Why this is vendored rather than fetched

The distributed package includes a corresponding-source archive built with
`git archive HEAD`, which contains tracked files only. libopus is statically
linked into the executable, so its source is part of the Corresponding Source
that GPL requires be offered alongside the binary. Fetching it at configure
time would leave that archive incomplete.

## Why 1.4 rather than 1.5

1.5 adds a neural deep-PLC and DRED, which together account for roughly 11 MB
of the tree. Both target low-bitrate speech through the SILK layer. JamLink
runs Opus in restricted low-delay mode, which is CELT only, at 5 ms frames —
none of that machinery is reachable, and carrying it would quadruple the size
of every clone and every corresponding-source archive for no benefit.

## What was left out

`tests/`, `doc/`, `m4/`, `meson/`, the autotools files, and the packaging
scripts. Everything the CMake build reads is present, including the `.mk`
source lists that `cmake/OpusSources.cmake` parses.

## Licence compatibility

BSD-3-Clause, with royalty-free patent grants recorded in
`LICENSE_PLEASE_READ.txt`. That is compatible with JamLink's current
GPL-3.0-or-later licence and, unlike the Steinberg ASIO SDK, would also be
compatible with a future proprietary edition without a separate agreement.
Attribution must be preserved either way.
