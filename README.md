# JamLink

JamLink is an early-stage Windows desktop audio project intended to make private, low-latency remote music sessions simple to start and trustworthy to use.

## Current status

This repository contains a tested Phase 0/1 engine foundation and the first native desktop shell. It is not yet a usable remote-jam application. The implemented scope includes:

- a C++20/CMake core with warnings treated as errors;
- a preallocated channel/bus route graph and bounded SPSC audio ring;
- gain, mute, coherent level metering, invalid-sample containment, and clock-drift control;
- a local-only Private Soundcheck processor and graph policy that rejects network/remote buses;
- fail-safe readiness invalidation and stable, atomic user-preference persistence;
- a Qt 6.10.3/QML Windows shell for Home, Private Sound Check, and Audio Settings;
- automated first-launch, second-launch, controller, offscreen visual, realtime-allocation, device-topology, hot-plug, integrity, stress, and virtual-time drift tests.

The production GUI does not invent devices or working meters: until a real Windows backend is connected it reports that audio backends are unavailable and disables dependent controls. A deterministic `--visual-fixture` exists only for automated screenshot and persistence tests.

There is no real-device backend, asynchronous resampler, networking, encryption, room, tuner, chat, or recording implementation yet. See [KNOWN_LIMITATIONS.md](KNOWN_LIMITATIONS.md).

## Build and test the core on Windows

Requirements:

- Windows 11 x64;
- CMake 3.25 or newer;
- Visual Studio 2022 with Desktop development with C++.

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-debug
ctest --preset windows-debug
```

## Build and test the desktop shell

Install the official Qt 6.10.3 MSVC 2022 x64 kit at `.qt/6.10.3/msvc2022_64`, or configure an equivalent build directory with `CMAKE_PREFIX_PATH` pointing to that exact kit. The checked preset keeps local Qt files ignored:

```powershell
cmake --preset windows-gui-vs2022
cmake --build --preset windows-gui-debug
ctest --preset windows-gui-debug
cmake --build build/windows-gui-vs2022 --config Debug --target jamlink_desktop_qmllint
```

Tests require no users, audio hardware, Internet connection, or downloaded test framework. GUI capture tests use Qt's offscreen software renderer and synthetic fixture data; they are not hardware or usability validation.

## Project principles

1. Real-time audio stability comes first.
2. Private Soundcheck remains structurally local-only.
3. Independent devices are independent clock domains.
4. User-facing measurements distinguish measured, estimated, and simulated values.
5. Planned features are never presented as working controls.
6. Qt stays on GUI/control threads and never enters `jamlink_core` or an audio callback.

## License

The primary JamLink application and repository are free software licensed under [GPL-3.0-or-later](LICENSE). The Qt desktop build selects Qt's GPL-3.0-only option; combined distributions that include Qt therefore must be conveyed under GPL version 3. See [NOTICE](NOTICE), [LICENSING.md](LICENSING.md), and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
