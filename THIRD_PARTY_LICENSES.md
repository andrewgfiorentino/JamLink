# Third-Party Licenses

## Material Design Icons

JamLink vendors ten Material Design Icons Round SVG files from Google at revision `50f0603134ce7b70b2d71b686cc13e8b57ccb74c` under Apache-2.0.

- Upstream: <https://github.com/google/material-design-icons>
- License: [`third_party/material-design-icons/LICENSE`](third_party/material-design-icons/LICENSE)
- Exact files, JamLink hashes, and modification notice: [`apps/desktop/assets/README.md`](apps/desktop/assets/README.md)

## Qt 6.10.3 pre-package developer/runtime inventory

The optional desktop target links an external official Qt 6.10.3 MSVC 2022 x64 kit. JamLink selects the `GPL-3.0-only` alternative from Qt's ordinary library expression `LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`. JamLink does not select a commercial Qt license or the LGPL alternative for this combined application.

Current module/runtime scope is Qt Core, Gui, Qml, Quick, QML Basic Controls, Quick Effects, Quick Layouts, and the Qt SVG image plugin. Qt Multimedia is excluded. The Basic style is pinned; optional Material, Fusion, Imagine, Universal, FluentWinUI3, native, and other Controls styles are outside the intended package.

Official source archive identities and hashes are recorded in [`DEPENDENCIES.md`](DEPENDENCIES.md) and [`dependencies.json`](dependencies.json). The installed kit's SPDX 2.3 SBOMs are authoritative for the build. Relevant bundled/attributed material identified there includes:

- Qt Core: BLAKE2, Double Conversion, easing equations, MD4/MD5/SHA implementations, RFC 6234, TinyCBOR, `tl::expected`, Apache Tika MIME data, and Unicode character/CLDR data under the exact CC0, Apache-2.0, BSD, MIT, public-domain-reference, and Unicode-3.0 expressions recorded by the SBOM;
- Qt Gui/Windows platform: AGLFN, D3D12 Memory Allocator, emoji-segmenter, FreeType gray raster, ICC sRGB data, md4c, OpenGL/Vulkan headers and allocator code, Qt RHI miniengine code, smooth scaling code, web gradients, zlib, and Wintab material under the SBOM's BSD, MIT, Apache-2.0, FTL/GPL-2.0-only, ICC, Imlib2, Zlib, HPND/X11, and `LicenseRef-Lcs-Telegraphics` expressions;
- bundled QtBase libraries: PCRE2 10.47, FreeType 2.14.3, HarfBuzz 13.2.1, libjpeg-turbo 3.1.4, libpng 1.6.56, and zlib 1.3.2 under their SBOM-recorded permissive or GPL-compatible alternatives;
- Qt QML: JavaScriptCore macro-assembler material under BSD-2-Clause;
- Qt Quick Layouts: Yoga 2.0.1 under MIT;
- Qt SVG: XSVG material under `HPND-sell-variant`.

This is a pre-package inventory, not the final deployed notice bundle. The repository does not distribute Qt DLLs/plugins or a packaged JamLink binary yet. Before that changes, the exact deployed file manifest must be generated and reviewed; all corresponding source, Qt and third-party notices/license texts, build configuration, and patches required by GPLv3 must accompany the distribution. Installation information is additionally required when GPLv3 section 6's User Product condition applies. Unused items present elsewhere in the full Qt kit are not automatically JamLink runtime dependencies.

Qt and the Qt logo are trademarks of The Qt Company Ltd. JamLink does not use the Qt logo.

## CI

The GitHub Actions workflow uses `actions/checkout` v7.0.1 at commit `3d3c42e5aac5ba805825da76410c181273ba90b1` under MIT. It runs only in CI and is not included in JamLink binaries.

## Build environment

CMake/CTest, the compiler, selected C++ standard library, Windows SDK libraries, Qt developer tools, and IDE are build-environment components and are not committed or redistributed by this source repository. Their licenses remain independently operative.

## ASIO notice

The ASIO SDK is not present, linked, or distributed yet. Its GPL-3.0-only path is an approved direction with an exact-archive ingestion gate. ASIO is a trademark of Steinberg Media Technologies GmbH.
