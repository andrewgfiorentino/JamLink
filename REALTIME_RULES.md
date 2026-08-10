# Real-Time Audio Rules

These rules apply to every hardware audio callback and every function reachable from it.

## Forbidden

- heap allocation or deallocation;
- blocking locks, waits, condition variables, or thread joins;
- file, console, database, DNS, socket, or UI operations;
- logging or string formatting;
- exceptions crossing the callback boundary;
- unbounded work;
- device enumeration, stream reconfiguration, or resampler construction.

## Required

- allocate and validate buffers before start;
- use bounded spans and fixed-capacity transfer structures;
- communicate telemetry through lock-free atomics or bounded SPSC structures;
- define underrun and overrun behavior explicitly;
- overwrite every output sample on every callback;
- contain NaN/Inf before processed audio reaches an output;
- make callback ownership and teardown order explicit;
- keep platform and third-party real-time calls auditable behind JamLink interfaces.

## Current enforcement

- real-time APIs are `noexcept`;
- floating control values use an always-lock-free 32-bit atomic representation;
- SPSC cursors and counters require always-lock-free 64-bit atomics on the x64 target;
- an automated ordinary/aligned allocation trap covers the current graph, gain, meter, Private Soundcheck, ring write/read, and simulated device-callback path;
- MSVC `/W4 /WX /permissive-` is enabled for JamLink targets.

The allocation trap supplements source review; it does not prove that every future library is real-time safe.
