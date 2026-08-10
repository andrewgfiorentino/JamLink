# ADR 0002: Direct ASIO and Qt 6 Direction

- Status: Qt adopted for the desktop shell; ASIO direction accepted but not ingested
- Date: 2026-08-10

## Context

The primary JamLink repository is `GPL-3.0-or-later`. The Windows local-audio milestone needs an ASIO path, and the later desktop application needs a modern GUI without allowing framework code into the real-time engine.

## Decision

Use the official Steinberg ASIO SDK directly behind `IAudioDeviceBackend` through its GPL version 3 open-source path. Do not use a framework audio wrapper. No SDK archive is committed until its official filename, retrieval date, SHA-256, exact included files, and per-file notices are recorded.

Use Qt 6.10.3 Quick/QML under Qt's GPL-3.0-only option for the first GUI. The minimal current scope is Core, Gui, Qml, Quick, Basic Controls, Quick Effects, Quick Layouts, and the SVG image plugin. Keep Qt types and runtime behavior outside `jamlink_core`; do not use Qt Multimedia for device access. Pin the Basic style and keep deterministic offscreen/software captures in the automated gate.

Do not adopt JUCE 9. Its open-source AGPLv3 obligations would change the conditions applied to a combined network application, and its commercial terms are not assumed to permit combination with GPL JamLink.

Use plain-text ASIO only for backend/device identification. Do not use the ASIO logo initially or incorporate ASIO into the JamLink product name.

## Consequences

- JamLink-owned source stays `GPL-3.0-or-later`.
- A combined distribution containing `GPL-3.0-only` ASIO or Qt code may need to be conveyed under GPL version 3 only while the original JamLink files retain their or-later grant.
- Exact Qt source identities/SHA-256 values and installed-kit SBOM names are inventoried. A binary package remains blocked until its deployment manifest, corresponding-source bundle, build information, and complete notices are reviewed.
- Device callbacks retain the no-allocation/no-lock/no-log/no-UI contract.
- Protocol, public SDK, and IPC/bridge boundaries remain eligible for an explicit permissive license when contributor and dependency rights allow it.

## Authoritative sources checked

- Steinberg ASIO open-source terms: <https://www.steinberg.net/developers/asiosdk-open/>
- GNU GPL version 3: <https://www.gnu.org/licenses/gpl-3.0.html>
- Qt 6 licensing: <https://doc.qt.io/qt-6/licensing.html>
- Qt open-source obligations: <https://www.qt.io/development/open-source-lgpl-obligations>
- JUCE license: <https://github.com/juce-framework/JUCE/blob/master/LICENSE.md>
