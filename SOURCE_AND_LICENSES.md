# Source and license information

JamLink-owned code is licensed under GPL-3.0-or-later. This Windows test package combines it with Qt 6.10.3 selected under GPL-3.0-only, so the combined executable is conveyed under GNU GPL version 3. `LICENSE`, `NOTICE`, and `THIRD_PARTY_LICENSES.md` are included beside the executable.

The exact JamLink corresponding source and build scripts are available at:

<https://github.com/andrewgfiorentino/JamLink>

Use the `v0.2.0-test` source tag. The package manifest allows the exact distributed files to be verified.

Official Qt 6.10.3 corresponding source archives used by this build:

- `qtbase-everywhere-src-6.10.3.zip` — SHA-256 `ddd7c0a3c798a8144a0fadb39c7c17b41cd5c55dd5caac22aca9ca3277b20024`
  <https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtbase-everywhere-src-6.10.3.zip>
- `qtdeclarative-everywhere-src-6.10.3.zip` — SHA-256 `6fe0d0e569c53effd8c82d6f57fb54a59b4914f2e52b9713035bcd1a2ccdf4fe`
  <https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtdeclarative-everywhere-src-6.10.3.zip>
- `qtsvg-everywhere-src-6.10.3.zip` — SHA-256 `5cf19e3d35524f17711511e5174a3b1316e60c94eeac0627a27b040c04af63c0`
  <https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtsvg-everywhere-src-6.10.3.zip>

JamLink does not modify Qt. The package is built from the official Qt 6.10.3 MSVC 2022 x64 binary kit with Qt Quick Basic controls and software rendering. Material Design Icons source provenance and Apache-2.0 notices are recorded in `THIRD_PARTY_LICENSES.md`; the full icon license is included as `MATERIAL_DESIGN_ICONS_LICENSE.txt`.

For a durable public release, JamLink will mirror complete corresponding Qt sources alongside the binary rather than relying only on upstream availability. This friend-test package is not represented as that final public release.
