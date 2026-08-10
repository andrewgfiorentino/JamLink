# Dependencies

## Adopted desktop dependencies

### Qt 6.10.3

The native desktop shell uses the official MSVC 2022 x64 Qt 6.10.3 kit. JamLink explicitly selects Qt's `GPL-3.0-only` option for the combined desktop application.

Used scope:

- Qt Core, Gui, Qml, and Quick;
- QML imports `QtQuick.Controls.Basic`, `QtQuick.Effects`, and `QtQuick.Layouts`;
- Qt SVG's image-format plugin decodes the project SVG icons;
- Qt Multimedia is not used.

Verified official source archives:

| Archive | SHA-256 |
|---|---|
| [`qtbase-everywhere-src-6.10.3.zip`](https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtbase-everywhere-src-6.10.3.zip) | `ddd7c0a3c798a8144a0fadb39c7c17b41cd5c55dd5caac22aca9ca3277b20024` |
| [`qtdeclarative-everywhere-src-6.10.3.zip`](https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtdeclarative-everywhere-src-6.10.3.zip) | `6fe0d0e569c53effd8c82d6f57fb54a59b4914f2e52b9713035bcd1a2ccdf4fe` |
| [`qtsvg-everywhere-src-6.10.3.zip`](https://download.qt.io/official_releases/qt/6.10/6.10.3/submodules/qtsvg-everywhere-src-6.10.3.zip) | `5cf19e3d35524f17711511e5174a3b1316e60c94eeac0627a27b040c04af63c0` |

The installed kit's `qtbase-6.10.3.spdx.json`, `qtdeclarative-6.10.3.spdx.json`, and `qtsvg-6.10.3.spdx.json` files were checked as the build-specific SBOMs. Qt is not committed to this repository and no redistributable application package is produced yet. Packaging remains blocked on a reviewed `windeployqt` manifest, exact binary-package identifiers/checksums, applicable post-release security patches, complete notices, the exact corresponding Qt sources, and a configuration/patch record. Installation information must also be supplied when GPLv3 section 6's User Product condition applies.

### Material Design Icons

Ten Material Design Icons Round SVG assets are vendored at revision `50f0603134ce7b70b2d71b686cc13e8b57ccb74c` under Apache-2.0. JamLink added a root white fill for deterministic Qt colorization without changing path geometry. Exact file hashes and the modification notice are in [`apps/desktop/assets/README.md`](apps/desktop/assets/README.md); the full license is in [`third_party/material-design-icons/LICENSE`](third_party/material-design-icons/LICENSE).

## Build requirements

- CMake 3.25 or newer, BSD-3-Clause;
- a conforming C++20 compiler and its standard library;
- Visual Studio 2022 x64 for the checked Windows presets;
- CTest, distributed with CMake.

## CI-only dependency

`actions/checkout` v7.0.1 is pinned at commit `3d3c42e5aac5ba805825da76410c181273ba90b1`, MIT. It obtains source in GitHub Actions and is not included in JamLink binaries.

## Approved direction, not yet adopted: Steinberg ASIO SDK

- Use the official SDK directly behind `IAudioDeviceBackend` under its GPL version 3 open-source path.
- Verified SDK code license expression: `GPL-3.0-only`, with a separate proprietary alternative that JamLink is not selecting.
- Before ingestion, record the official archive's actual filename, URL, retrieval date, SHA-256, included files, and every embedded notice. No semantic SDK version is asserted until the official archive identifies one.
- Preserve notices, mark/date changes, and provide complete corresponding source/build material with distributed binaries.
- Use plain-text ASIO only for device/backend identification; do not use the logo or include ASIO in the product name.

## Not selected

JUCE 9 is not selected. Its open-source framework license is AGPLv3, whose network-source conditions would extend beyond the chosen GPL posture for a combined application. Its commercial terms are not assumed to be compatible with a GPL build.

## Admission policy

Project-license compatibility alone never approves a dependency. Admission requires an exact version or revision, authoritative license data, transitive inventory, notice/source obligations, and a realtime audit before any callback-path use. Machine-readable metadata is maintained in [`dependencies.json`](dependencies.json).

Reusable protocol, SDK, and IPC components may receive an explicit permissive license only when their contributors and dependencies permit it. The main application remains GPL-3.0-or-later.
