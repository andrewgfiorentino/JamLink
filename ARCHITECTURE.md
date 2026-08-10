# Architecture

## Status

The implemented code is the Phase 0/1 engine foundation plus the first native desktop shell. Platform audio, room/session, transport, security, recording, tuner, and DAW bridge modules are not implemented.

## Module boundaries

- `jamlink::audio`: audio blocks, device-backend contract, compiled route graph, real-time gain/metering, local Private Soundcheck, and SPSC audio transfer.
- `jamlink::clock`: relative clock-rate telemetry and bounded ring-fill correction.
- `jamlink::control`: non-real-time readiness and safe room-entry state.
- `jamlink::preferences`: versioned, validated, atomically replaced user preferences containing stable device/channel identifiers and window state; readiness is intentionally not persisted.
- `apps/desktop`: Qt 6.10.3 GUI and controller on the UI/control thread. It depends on `JamLink::Core`; the core has no Qt dependency.
- `tests/support`: deterministic virtual device behavior used only by automated tests.

Platform audio APIs must remain behind `IAudioDeviceBackend`. The rest of the core must not depend on WASAPI, ASIO, a GUI framework, or a future transport library.

## Thread model

The intended ownership model is:

- one real-time callback owner per opened hardware clock domain;
- non-real-time device/session control;
- future network send and receive workers;
- future recording/file workers;
- the current Qt UI/control thread.

`SpscAudioRing` is the current bounded transfer primitive between one producer and one consumer. Bus graphs and controllers are single-owner objects; they are prepared before processing begins.

## Clock domains

Every independently clocked input or output device receives a distinct `clockDomainId`. Ring occupancy provides the short-term control signal. `ClockDriftEstimator` reports relative frame-rate error, and `ClockDomainController` produces a bounded input-frames-per-output-frame ratio for a future asynchronous resampler. Controller integration and smoothing use elapsed frames and nominal sample rate, so response is not tied to callback block size.

The controller is implemented and tested. An audio resampler is not yet integrated, so different real devices cannot yet be aggregated.

## Licensing and interoperability boundaries

The primary application is `GPL-3.0-or-later`. Public protocol specifications, interoperability SDKs, and IPC/bridge libraries must remain narrow modules with no dependency on UI or device implementation details. If a future component benefits from MIT or Apache-2.0 licensing, it must be separately identified and licensed without changing the main application's GPL status.

Transport and device interfaces are architectural seams, not license loopholes. Every linked or distributed dependency still requires an exact version/license/obligation review.

The desktop shell uses Qt 6.10.3 under its GPL-3.0-only option. Qt types, containers, event loops, rendering, and Qt Multimedia do not enter `jamlink_core` or an audio callback. The current controller exposes synthetic level values only under an explicit automated visual-fixture flag; production mode reports the absent backend and disables device-dependent controls. Future real meters must consume bounded core telemetry snapshots on the UI thread.

Qt Quick's Basic Controls style and software-rendered automated captures are pinned for repeatability. QML assets are compiled into the executable, while the Qt runtime remains an external developer dependency until the release deployment/notices gate is implemented. The ASIO direction remains the official SDK used directly behind `IAudioDeviceBackend`; discovery, loading, control panels, start/stop, and recovery stay on control threads.

## Private Soundcheck boundary

`PrivateSoundcheckProcessor` accepts local instrument and voice spans and has no transport handle, socket, or session object. `AudioRouteGraph` compiled for `PrivateSoundcheck` rejects network and remote bus roles, so future graph assembly cannot attach the local path to a NetworkSend bus.

Room transmission will be implemented as a separate component. A future join transition must consume `ReadinessTracker::joinSafetyDecision()` and begin unverified sources muted.

## Desktop state boundary

`AppController` is an uncreatable typed QML element instantiated by the application. It owns navigation, device-selection presentation, readiness invalidation, and delayed control-thread persistence. QML has no audio-backend, transport, socket, or callback access. The implemented pages are Home, Private Sound Check, and Audio Settings; room, tuner, recording, and chat controls are absent because their functional paths do not exist.

## Configuration lifecycle

Device enumeration, graph construction, buffer allocation, stream opening, and error construction occur on control threads. The real-time phase operates on fixed spans, preallocated vectors, atomics known to be lock-free on the x64 target, and bounded loops.
