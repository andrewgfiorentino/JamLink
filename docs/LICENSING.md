# Licensing

## Primary application

JamLink is free software licensed under `GPL-3.0-or-later`. Unless a file explicitly carries a different SPDX identifier, repository source and build assets are covered by that license. See `LICENSE` and `../NOTICE`.

Contributions require a signed Contributor Licence Agreement before they can be merged. See `CONTRIBUTING.md` and `CLA.md`.

A contribution offered under `GPL-3.0-or-later` alone is not sufficient, because GPL does not permit the project to relicense the contributed work. JamLink has a single copyright holder, and that is the only reason a dual-licensed or commercial edition remains possible; a single contribution merged without the agreement would remove that permanently and could not be undone afterwards. `licensing-options.md` records what the current dependencies allow.

## Separable interoperability work

Separately identified protocol specifications, public interoperability headers, SDKs, or IPC libraries may use an explicit permissive license such as MIT or Apache-2.0 only when every contributor and included dependency permits it. No component becomes permissively licensed merely because it is reusable. The primary JamLink application remains `GPL-3.0-or-later`.

JamLink-owned files retain their `GPL-3.0-or-later` grant. A distributed combined binary may have to be conveyed under GPL version 3 only when it incorporates a dependency offered under `GPL-3.0-only`; that narrower combined-work condition does not relicense the original JamLink files.

The current Qt 6.10.3 desktop build explicitly selects Qt's `GPL-3.0-only` alternative, and the included Steinberg ASIO SDK interface files select GPL version 3. Any distributed combined desktop binary must therefore be conveyed under GPL version 3 with complete corresponding source and the required Qt/ASIO/third-party notices. No JamLink-owned file loses its `GPL-3.0-or-later` grant.

## Third-party dependencies

Project-license compatibility is only one admission check. Every dependency must still be reviewed for its exact version, complete license set, notices, source/conveyance requirements, trademarks, patents, transitive components, platform restrictions, and real-time behavior before adoption.
