# Changelog

## Unreleased

### Added

- GPL-3.0-or-later project license, SPDX source notices, and separable interoperability licensing policy.
- Direct-ASIO and Qt 6 GPLv3 dependency directions with exact-ingestion gates.
- Pinned Qt 6.10.3/QML desktop shell under Qt's GPL-3.0-only option.
- Native Home, Private Sound Check, and Audio Settings screens aligned to the approved visual reference without nonfunctional room/tuner/record/chat controls.
- Versioned, validated, atomically replaced preferences with first/second-launch restoration by stable device/channel identifiers.
- Deterministic GUI controller tests, offscreen high-DPI visual captures, and a clean QML lint target.
- Vendored Material Design Icons Round assets with exact revision, hashes, modification notice, and Apache-2.0 license.
- C++20/CMake build and Windows presets.
- Initial audio-device backend contract with explicit channel selection and capability reporting.
- Preallocated channel/bus route graph.
- Lock-free SPSC interleaved audio ring with underrun/overrun telemetry.
- Smoothed gain/mute, peak/RMS/clip metering, and invalid-sample containment.
- Local-only Private Soundcheck processor.
- Readiness invalidation and safe join-mute decisions.
- Clock-drift estimator and bounded occupancy controller.
- Deterministic simulated-device, integrity, allocation, hot-plug, virtual-time, and stress tests.
- Project status, architecture, real-time, test, benchmark, dependency, security, and limitation documentation.
