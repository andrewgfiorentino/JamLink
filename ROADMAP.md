# Roadmap

Each phase is gated by automated evidence. Later work must not be presented as complete while an earlier audio-foundation gate is open.

## Current — Phase 0/1 foundation and desktop shell

- [x] C++20/CMake project and warning policy.
- [x] Provisional audio-device interface with explicit channel selection and capability reporting.
- [x] Preallocated channel/bus routing core.
- [x] SPSC audio transfer, metering, gain/mute, readiness safety.
- [x] Clock drift estimator and occupancy controller.
- [x] Private Soundcheck processor without transport dependencies and graph policy rejecting network buses.
- [x] Deterministic virtual device and long-run clock tests.
- [x] License the primary repository as GPL-3.0-or-later.
- [x] Select direct ASIO SDK and Qt 6 GPLv3 integration directions; reject JUCE 9.
- [x] Pin Qt 6.10.3 modules/source hashes/SBOMs and adopt the GPL-3.0-only option.
- [x] Implement stable, versioned, atomic first/second-launch preference persistence.
- [x] Implement the first native Home, Private Sound Check, and Audio Settings GUI from the approved visual reference.
- [x] Add deterministic controller, persistence, high-DPI offscreen screenshot, and clean QML lint gates.
- [ ] Pin and inventory the exact official ASIO archive before ingestion.
- [ ] Produce and audit a Windows Qt deployment manifest, corresponding-source bundle, and complete runtime notices before packaging.
- [ ] Implement direct WASAPI Shared and Exclusive backends.
- [ ] Add asynchronous resampling and multi-device aggregation.
- [ ] Automate device lifecycle, format, cadence, race, and leak matrices.
- [ ] Implement offline Soundcheck output test and temporary test recording.

## Next gates

1. Complete and verify the Windows local-audio engine with simulated platform seams.
2. Add tuner analysis without inserting latency into the music path.
3. Build a headless transport and network-impairment lab.
4. Implement the encrypted two-person private-room MVP.
5. Harden reconnection, NAT traversal, adaptive buffering, and relay fallback.

Recording, larger rooms, advanced routing, DAW bridge, and synchronized remote masters follow only after their prerequisites are measured and stable.
