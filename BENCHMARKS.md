# Benchmarks

All results below are engineering measurements of synthetic code paths. They are not hardware latency, audio round-trip, LAN, WAN, or subjective audio-quality results.

## 2026-08-10 — core processing stress

Environment:

- Windows x64;
- MSVC 19.44.35228;
- CMake 4.3.1;
- Release configuration;
- synthetic 48 kHz, 128-frame instrument and voice blocks;
- local Private Soundcheck mix, metering, SPSC write/read, and clock-controller update.

Result:

| Metric | Value |
|---|---:|
| Simulated audio time | 3,600 s |
| Wall time | 3.48902 s |
| Processing factor | 1031.81× real time |
| SPSC underruns | 0 |
| SPSC overruns | 0 |
| Final synthetic peak | -6.02108 dBFS |

This run used the current warnings-as-errors Release GUI build's headless stress executable in one process with synthetic in-memory audio. It does not validate device drivers, scheduler behavior at real callback cadence, separate hardware clocks, Qt rendering contention at wall-clock cadence, or end-to-end latency.
