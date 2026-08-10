# Architecture

## Implemented milestone

The repository now contains a two-person Windows test slice spanning devices, clock conversion, Private Soundcheck, encrypted direct transport, invite creation/joining, and a packaged Qt desktop UI. The slice is intentionally narrow: Windows WASAPI Shared, IPv4, two peers, and mono network audio.

## Module boundaries

- `jamlink::audio`: prepared route graphs, gain/metering, Private Soundcheck, bounded SPSC rings, and allocation-free asynchronous mono resampling.
- `jamlink::clock`: relative clock-rate telemetry and bounded ring-fill correction.
- `jamlink::control`: readiness invalidation and safe room-entry decisions.
- `jamlink::preferences`: versioned, validated, atomically replaced device/channel and window preferences. Readiness is never persisted.
- `jamlink::network`: Qt-free peer exchange and transport interfaces.
- `src/platform/windows`: direct WASAPI Shared device service plus Winsock/CNG/UPnP/STUN peer transport.
- `apps/desktop`: Qt 6.10.3 GUI/controller. QML receives bounded telemetry and cannot access device callbacks, sockets, or cryptographic handles.

The provisional `IAudioDeviceBackend` remains the future full ASIO/WASAPI backend seam. The tonight-test service is the narrower `ISoundcheckAudioService`, which keeps all Windows API types behind the platform target.

## Thread ownership

- the Qt UI/control thread owns selection, lifecycle, persistence, navigation, and telemetry polling;
- one Windows Multimedia Class Scheduler worker owns the three WASAPI clients and all local audio processing;
- one network worker owns Winsock, address discovery, UPnP, AES-GCM, packet parsing, handshake, and timing;
- audio crosses between those workers only through bounded SPSC sample rings;
- no socket, DNS, encryption, COM setup, string, Qt, or logging operation is reachable from the audio loop.

The audio service is stopped before the peer exchange pointer changes. On leave, audio stops, the pointer is cleared, the network worker stops, and only then is Private Soundcheck restarted.

## Local clock domains

Instrument capture, voice capture, render, and network playback are treated as separate clocks. Each capture stream writes a preallocated ring. `AsyncMonoResampler` converts each to the render cadence while a bounded occupancy feedback term corrects long-term drift. Remote 48 kHz audio is converted to the selected output rate in the same worker. The interpolation is deliberately simple for this first test and still needs quality benchmarking.

## Private Soundcheck isolation

The application does not construct `IPeerAudioTransport` until the user explicitly chooses Create Invite or Join. Private mode gives the audio service a null `IPeerAudioExchange`; the core Private Soundcheck graph also rejects every network and remote bus role. Automated tests cover that graph policy. A production transport spy/state-transition matrix remains a later hardening gate.

## Room boundary

Hosting creates a UDP socket, a random room key, an optional same-port UPnP mapping, and a `JL1` invite. Joining parses the invite, pins the host endpoint, and begins an authenticated handshake. Once connected, the audio worker pushes a bounded guitar+voice mono mix to the transport ring and mixes the decoded remote mono stream into local output. Room mute suppresses transport input without changing local monitor controls.

## Licensing and interoperability

The main application remains GPL-3.0-or-later. Narrow protocol specifications, SDKs, IPC/bridge libraries, and interoperability headers must remain separable so they can potentially receive an explicit MIT or Apache-2.0 license later. The current transport implementation is GPL-3.0-or-later; no current protocol file is implicitly permissive.

Qt stays outside the engine and selects GPL-3.0-only. Windows system APIs are called directly. The ASIO SDK remains approved for later exact-archive ingestion under GPL-3.0-only and is not in this tree.
