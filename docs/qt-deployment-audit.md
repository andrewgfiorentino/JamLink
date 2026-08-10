# Qt Deployment Audit

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

## Status

The desktop target is buildable and testable from the official Qt 6.10.3 MSVC 2022 x64 kit. JamLink does not yet produce or publish a redistributable Windows package.

## 2026-08-10 dry run

Qt's `windeployqt` was run in `--release --dry-run --list relative` mode against the warnings-as-errors Release executable with `--qmldir apps/desktop/qml` and translations disabled. The scanner identified the expected Qt Core/Gui/Qml/Quick, Basic Controls, Quick Effects, Quick Layouts, Quick Shapes/Templates, SVG, and Windows platform/image plugin families.

It also proposed broader transitive/runtime material including Qt Network/TLS plugins, QML debugging tooling, software OpenGL/D3D support files, and generic plugins. The dry run warned that `dxcompiler.dll`/`dxil.dll` were unavailable and that `VCINSTALLDIR` was not set.

## Package gate

No deployment output from that dry run is a release artifact. Before packaging:

1. generate a full manifest in a clean Visual Studio developer environment;
2. remove QML debugging/tooling and other unused plugins through a tested deployment allowlist rather than ad-hoc deletion;
3. decide and test the supported renderer/runtime files and resolve the DirectX compiler warning;
4. run the packaged application and automated first/second-launch captures without access to the developer Qt tree;
5. inventory every shipped DLL/plugin against the installed-kit SPDX SBOMs;
6. record exact Qt binary-package identifiers/checksums and every applicable post-release patch;
7. resolve the stock Qt 6.10.3 SVG package against Qt's [CVE-2026-6210 advisory](https://www.qt.io/blog/security-advisory-type-confusion-and-heap-buffer-overflow-vulnerability-in-qt-svg-marker-handling) before accepting SVG parsing in a release package;
8. bundle the exact corresponding Qt sources, build/configuration information, required license texts, attribution notices, and—when GPLv3 section 6's User Product condition applies—installation information.

Until this gate closes, documentation must call the GUI a source-build desktop shell, not a distributable application.
