# Licensing

## Primary application

JamLink is free software licensed under `GPL-3.0-or-later`. Unless a file explicitly carries a different SPDX identifier, repository source and build assets are covered by that license. See `LICENSE` and `NOTICE`.

Contributions to GPL-covered files are expected to be provided under `GPL-3.0-or-later` unless a separate written agreement says otherwise.

## Separable interoperability work

Protocol specifications, public interoperability headers, SDKs, IPC/bridge libraries, and compatibility tools should be designed as narrow, reusable boundaries with minimal application dependencies. When third-party adoption would benefit, a future component may be placed in a clearly separate directory or repository and explicitly licensed under a permissive license such as MIT or Apache-2.0.

No component becomes permissively licensed merely because it is reusable. A different license requires an explicit SPDX identifier, complete license text/notice, and confirmation that every contributor and included dependency permits that choice. The primary JamLink application remains `GPL-3.0-or-later`.

JamLink-owned files retain their `GPL-3.0-or-later` grant. A distributed combined binary may have to be conveyed under GPL version 3 only when it incorporates a dependency offered under `GPL-3.0-only`; that narrower combined-work condition does not relicense the original JamLink files.

The current Qt 6.10.3 desktop build explicitly selects Qt's `GPL-3.0-only` alternative. Any distributed combined desktop binary must therefore be conveyed under GPL version 3 with complete corresponding source and the required Qt/third-party notices. No JamLink-owned file loses its `GPL-3.0-or-later` grant.

## Third-party dependencies

Project-license compatibility is only one admission check. Every dependency must still be reviewed for its exact version, complete license set, notices, source/conveyance requirements, trademarks, patents, transitive components, platform restrictions, and real-time behavior before adoption.
