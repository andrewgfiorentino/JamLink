# JamLink

JamLink is a free, open-source Windows desktop application for private two-person remote music tests.

## Current testable scope

Version 0.2.0 is an early Windows 11 x64 test build, not a production release. The implemented end-to-end path includes:

- real WASAPI Shared capture and output with independent guitar, microphone, and output selection;
- asynchronous conversion between independent input and output clock domains;
- low-latency local monitoring, live meters, gain/mute, and a quiet output test;
- local-only Private Soundcheck before hosting or joining;
- a working `JL1` invite code containing a public IPv4 address, UDP port, and random 256-bit room secret;
- automatic UPnP port mapping, public-address discovery through Cloudflare's public STUN endpoint, and a displayed manual UDP-port fallback;
- authenticated AES-256-GCM direct peer handshake with per-direction keys, a sliding-window replay filter, and endpoint pinning;
- independent instrument and voice streams, each with its own receive buffer and its own remote level and mute, so one can be turned down without the other;
- an adaptive receive jitter buffer with pitch-synchronous packet-loss concealment and bounded latency growth;
- room mute, packet counters, per-stream meters, and a connection summary that separates measured round trip from estimated one-way delay;
- a chromatic tuner tapped off the instrument input, with a tuner mute that silences the instrument to the room while voice keeps flowing;
- one-button recording that writes four sample-aligned 32-bit float WAV tracks — your instrument, your voice, and each of your friend's streams — from a dedicated disk worker;
- a self-contained Windows ZIP, including its exact JamLink source archive, produced by `scripts/package_windows.ps1`.

The invite system is functional, not a visual placeholder. Automated tests create a host and guest on real loopback UDP sockets, complete the encrypted handshake, exchange nonzero audio on both streams in both directions, confirm that muting one stream leaves the other audible, reflect the host's own packets back at it from its pinned endpoint, and flood a live session with malformed datagrams. A development-machine automation also opened a real Focusrite WASAPI combination. Those checks are not live-user, cross-home-network, subjective, or hardware-compatibility validation.

The jitter buffer and concealment are measured against a deterministic impairment model in `tests/jamlink_network_tests.cpp`, not against a real Internet path. Over one virtual hour at 1.1% channel loss with 25 ms latency, 8 ms jitter, bursts, and reordering, concealment tracked real loss at 0.87% and receive depth peaked at 65 ms. Isolated-loss concealment measured 14.8 dB below zero fill. The tuner measures within 0.16 cents from a 31 Hz bass fundamental to a 1319 Hz fretted guitar note, including when the fundamental is missing entirely.

Read [TONIGHT_TEST.md](TONIGHT_TEST.md) before testing across two homes. The most important limitation is that this build has no relay: UPnP or manual UDP forwarding may be required, and carrier-grade or symmetric NAT can still prevent connection. Sending instrument and voice separately also roughly doubles upstream bandwidth to about 1.6 Mbit/s, because there is still no codec.

## Build and test on Windows

Requirements:

- Windows 11 x64;
- CMake 3.25 or newer;
- Visual Studio 2022 with Desktop development with C++;
- the official Qt 6.10.3 MSVC 2022 x64 kit at `.qt/6.10.3/msvc2022_64`.

```powershell
cmake --preset windows-gui-vs2022
cmake --build --preset windows-gui-debug
ctest --preset windows-gui-debug
cmake --build build/windows-gui-vs2022 --config Debug --target jamlink_desktop_qmllint

cmake --build --preset windows-gui-release
ctest --preset windows-gui-release
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1
```

The final command writes the untracked friend-test folder and ZIP under `dist/`.

## License

The primary JamLink application and repository are free software licensed under [GPL-3.0-or-later](LICENSE). The packaged Qt desktop selects Qt's GPL-3.0-only option; the combined executable is conveyed under GPL version 3. See [NOTICE](NOTICE), [LICENSING.md](LICENSING.md), [SOURCE_AND_LICENSES.md](SOURCE_AND_LICENSES.md), and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
