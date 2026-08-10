# Testing

JamLink validation is automated. The current gate does not depend on live users, manual listening, physical loopback cables, attached audio devices, or Internet peers.

## Commands

```powershell
cmake --preset windows-vs2022
cmake --build --preset windows-debug
ctest --preset windows-debug
```

With the official Qt 6.10.3 MSVC 2022 x64 kit installed at `.qt/6.10.3/msvc2022_64`:

```powershell
cmake --preset windows-gui-vs2022
cmake --build --preset windows-gui-debug
ctest --preset windows-gui-debug
cmake --build build/windows-gui-vs2022 --config Debug --target jamlink_desktop_qmllint
```

The equivalent Release checks use the `windows-release` and `windows-gui-release` build/test presets.

For a longer accelerated processing run after a Release build:

```powershell
cmake --build --preset windows-release
build\windows-vs2022\tools\core_stress\Release\jamlink_core_stress.exe 1350000
```

At 48 kHz and 128 frames per block, `1,350,000` blocks represent one hour of simulated audio time. Simulated time is not real hardware uptime.

## Current automated coverage

- SPSC wraparound, order, underflow zero-fill, and telemetry;
- concurrent SPSC producer/consumer sequence integrity;
- channel/polarity preservation and graph fan-out;
- graph topological ordering, cycle rejection, and sample-shape overflow rejection;
- gain smoothing, mute, peak/RMS, clip latch, and NaN/Inf containment;
- structural local-only Private Soundcheck processing;
- zero observed allocation across current real-time paths;
- readiness invalidation and safe join-mute decisions;
- simulated callback processing, device removal, enumeration change, reopen, and recovery;
- scripted one-device, separate-microphone, and independent-output topologies with explicit channel maps;
- bounded positive and negative clock correction;
- known drift estimation;
- virtual eight-hour `-100 ppm` and `+100 ppm` drift simulations;
- callback-cadence invariance at 44.1/48/96 kHz and 64/128/512 frames;
- accelerated core stress with deterministic synthetic signals.
- versioned preference defaulting, stable device/channel restoration, atomic replacement, and corrupt-file recovery;
- GUI controller navigation, selection persistence, readiness invalidation, and explicit Sound Check save;
- sequential first-launch/second-launch Home, Sound Check, and Settings captures using the offscreen software renderer;
- QML static lint with typed controller properties.

The normal screenshot gate requests a 532 × 534 logical-pixel viewport. On the current Windows 125% scale it captures 665 × 668 physical pixels, which is expected high-DPI behavior. A separate test overrides the offscreen screen factor to exactly 1.5 and requires an exact 798 × 801 physical-pixel capture. Captures use deterministic fixture labels/levels and do not count as live-device, user, accessibility, or subjective visual validation.

## Required before the local-audio gate closes

- direct WASAPI backend contract tests with platform seams/mocks;
- scripted shared/exclusive open failures and device-format changes;
- non-power-of-two callback blocks and split/merged device cadence;
- asynchronous resampler quality and long-run drift tests;
- repeated concurrent start/stop/teardown tests;
- automated leak and race instrumentation;
- automated 44.1, 48, 88.2, and 96 kHz signal-integrity matrices.
