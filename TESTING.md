# Testing

JamLink's repository gate is fully automated. No test depends on live users, manual listening, physical loopback cables, attached audio hardware, or an Internet peer.

## Commands

```powershell
cmake --preset windows-gui-vs2022
cmake --build --preset windows-gui-debug
ctest --preset windows-gui-debug
cmake --build build/windows-gui-vs2022 --config Debug --target jamlink_desktop_qmllint

cmake --build --preset windows-gui-release
ctest --preset windows-gui-release
```

Long accelerated core run:

```powershell
build\windows-gui-vs2022\tools\core_stress\Release\jamlink_core_stress.exe 1350000
```

At 48 kHz and 128 frames/block, 1,350,000 blocks represent one simulated hour. Simulated time is not real hardware uptime.

## Automated coverage

- SPSC wrap, underflow zero-fill, concurrent producer/consumer order and sequence integrity;
- route identity, fan-out, topological ordering, cycle/shape rejection;
- smoothed gain/mute, coherent peak/RMS, clip latch, NaN/Inf containment;
- zero observed ordinary/aligned allocation across current audited audio primitives;
- Private Soundcheck graph rejection of all remote/network roles;
- readiness invalidation and safe join-mute decisions;
- scripted device topologies, removal/reopen, format/cadence variation, and deterministic recovery;
- clock estimation/correction matrices and virtual eight-hour +/-100 ppm simulations;
- allocation-free 44.1-to-48 kHz conversion and capture-drift resampler simulations;
- encrypted host/guest handshake on real loopback UDP sockets;
- authenticated bidirectional nonzero PCM audio exchange and packet telemetry;
- versioned preference restoration, stale-device routing, atomic replacement, and corrupt-file recovery;
- typed QML controller, first/second launch routing, readiness, live-backend telemetry wiring, and persistence;
- deterministic Home, Sound Check, and Settings captures, including exact 150% scaling;
- QML static lint.

The peer test proves the invite parser, socket handshake, AES-GCM packet path, bidirectional audio queues, and state progression within one Windows host. It does not prove UPnP behavior on a given router, cross-home reachability, WAN jitter/loss behavior, firewall acceptance, or live-user audio quality.

The normal screenshot gate uses a 532 x 534 logical viewport. On the current Windows 125% host it captures 665 x 668 physical pixels. The explicit 150% test neutralizes native scaling and requires exactly 798 x 801. Fixture screenshots do not count as device or usability validation.

## Manual/live status

A screenshot-driven automation successfully enumerated and opened a Focusrite WASAPI Shared combination at 96 kHz on the development machine after correcting the IAudioClient3 flag set. This is useful local evidence, but it is not part of the hardware-independent CTest gate and is not described as latency or stability validation.

## Still required for production

- direct WASAPI seam/failure injection and repeated concurrent teardown;
- sanitizer/race/leak instrumentation and hostile packet fuzzing;
- signal-integrity matrices at 44.1/48/88.2/96 kHz and resampler quality benchmarks;
- controlled network loss, jitter, reorder, duplicate, and bandwidth tests;
- automated NAT/UPnP integration lab and authenticated relay tests;
- code signing, installer/update security, accessibility automation, and public package compliance audit.
