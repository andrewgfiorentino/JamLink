# Source and license information

This document describes the preserved **GPL test-package path** for JamLink releases published through `v0.4.8-test`.

JamLink-owned code in this public release line is licensed under
GPL-3.0-or-later. The Windows GPL test package combines it with Qt 6.10.3
selected under GPL-3.0-only and Steinberg ASIO SDK interface files selected
under GPL version 3, so that combined executable is conveyed under GNU GPL
version 3. `LICENSE`, `../NOTICE`, `THIRD_PARTY_LICENSES.md`, and the applicable
ASIO SDK license information accompany that package.

The GPL package script creates the exact JamLink corresponding source and build
scripts beside the executable as `JamLink-<package-version>-source.zip` using
`git archive HEAD`. `SOURCE_COMMIT.txt` identifies the exact Git commit used to
generate the package, and `PACKAGE_MANIFEST.sha256` records every distributed
file.

Public JamLink source/releases published through `v0.4.8-test` remain available
from this repository. The preservation branch
`gpl-public-snapshot-2026-08-19` identifies the end of continuously published
active-development source. Rights already granted under GPL are unchanged.

Official Qt 6.10.3 corresponding source archives used by this build:

- `qtbase-everywhere-src-6.10.3.zip` — SHA-256 `ddd7c0a3c798a8144a0fadb39c7c17b41cd5c55dd5caac22aca9ca3277b20024`
  <https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtbase-everywhere-src-6.10.3.zip>
- `qtdeclarative-everywhere-src-6.10.3.zip` — SHA-256 `6fe0d0e569c53effd8c82d6f57fb54a59b4914f2e52b9713035bcd1a2ccdf4fe`
  <https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtdeclarative-everywhere-src-6.10.3.zip>
- `qtsvg-everywhere-src-6.10.3.zip` — SHA-256 `5cf19e3d35524f17711511e5174a3b1316e60c94eeac0627a27b040c04af63c0`
  <https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtsvg-everywhere-src-6.10.3.zip>

JamLink does not modify Qt. The GPL package is built from the official Qt
6.10.3 MSVC 2022 x64 binary kit with Qt Quick Basic controls and software
rendering.

Material Design Icons source provenance and Apache-2.0 notices are recorded in
`THIRD_PARTY_LICENSES.md`; the full icon license is included as
`MATERIAL_DESIGN_ICONS_LICENSE.txt`.

Opus 1.4 provenance, BSD redistribution terms, and patent-grant references are
recorded in `THIRD_PARTY_LICENSES.md`, `dependencies.json`, and the files under
`third_party/opus`.

Active JamLink development now occurs privately. Any future proprietary JamLink
package uses a separate compliance path and does not alter the license of the
source or releases preserved here.
