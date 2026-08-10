# JamLink

JamLink is a free, open-source Windows desktop application for private two-person remote music tests.

## Current testable scope

Version 0.2.0 is an early Windows 11 x64 test build, not a production release. The implemented end-to-end path includes:

- real WASAPI Shared capture and output with independent guitar, microphone, and output selection;
- asynchronous conversion between independent input and output clock domains;
- low-latency local monitoring, live meters, gain/mute, and a quiet output test;
- fail-safe Private Soundcheck that constructs no transport or socket;
- a working `JL1` invite code containing a public IPv4 address, UDP port, and random 256-bit room secret;
- automatic UPnP port mapping, public-address discovery through Cloudflare's public STUN endpoint, and a displayed manual UDP-port fallback;
- authenticated AES-256-GCM direct peer handshake and bidirectional 48 kHz mono PCM audio;
- replay rejection, endpoint pinning, room mute, packet counters, remote meter, and network round-trip display;
- a self-contained Windows ZIP, including its exact JamLink source archive, produced by `scripts/package_windows.ps1`.

The invite system is functional, not a visual placeholder. Automated tests create a host and guest on real loopback UDP sockets, complete the encrypted handshake, and exchange nonzero audio in both directions. A development-machine automation also opened a real Focusrite WASAPI combination. Those checks are not live-user, cross-home-network, subjective, or hardware-compatibility validation.

Read [TONIGHT_TEST.md](TONIGHT_TEST.md) before testing across two homes. The most important limitation is that this build has no relay: UPnP or manual UDP forwarding may be required, and carrier-grade or symmetric NAT can still prevent connection.

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

## Project principles

1. Real-time audio stability comes first.
2. Private Soundcheck remains structurally local-only.
3. Independent devices are independent clock domains.
4. User-facing measurements distinguish measured, estimated, and simulated values.
5. Planned features are never presented as working controls.
6. Qt stays on GUI/control threads and never enters the audio processing path.
7. Network sockets and cryptography stay on the network worker, never on the audio thread.

## License

The primary JamLink application and repository are free software licensed under [GPL-3.0-or-later](LICENSE). The packaged Qt desktop selects Qt's GPL-3.0-only option; the combined executable is conveyed under GPL version 3. See [NOTICE](NOTICE), [LICENSING.md](LICENSING.md), [SOURCE_AND_LICENSES.md](SOURCE_AND_LICENSES.md), and [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md).
