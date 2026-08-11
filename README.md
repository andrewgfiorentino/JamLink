# JamLink

JamLink is a free, open-source Windows application for private two-person remote music sessions.

Version 0.3.0-test provides:

- native ASIO instrument/output with a separately synchronized WASAPI USB microphone, plus all-WASAPI fallback;
- local Private Sound Check with device/channel selection, monitoring, meters, mute, gain, and output test;
- persistent per-source clipping warnings with peak hold, input/send/mix diagnosis, and callback-safe reset;
- encrypted `JL1` invite codes for direct two-person Internet rooms, including automatic UPnP mapping and public-address discovery;
- independent instrument and voice streams with adaptive jitter buffering, packet-loss concealment, and separate remote mix controls;
- authenticated participant identity, exact-build compatibility checks, reliable private room chat, reconnect handling, and malformed-packet protection;
- persistent musician profiles, built-in/custom avatars, tuner, and four-track aligned recording;
- an in-app updater backed by matching GitHub Release ZIP and SHA-256 assets;
- a self-contained Windows ZIP with the exact corresponding source and license notices.

The invite/audio/chat path is covered by automated real-socket loopback tests. The mixed audio backend was also opened automatically on a development PC using Focusrite USB ASIO Input 2 and Outputs 1–2 while a separate WASAPI microphone ran and was replaced without stopping ASIO. These checks do not replace the first live two-home listening test.

Read [TONIGHT_TEST.md](TONIGHT_TEST.md) for the short two-person test procedure.

Signal-health labels use unsmoothed sample peaks: below -40 dBFS is **Too Quiet**, -40 to -6 dBFS is **Good**, -6 to -1 dBFS is **Hot**, and -1 dBFS to full scale is **Near Clip**. Native input full-scale codes (at or above 0.9999 linear) and internal values at or above 1.0 latch **Clipping**. The meter animation may decay, but the clip latch and peak hold remain until reset.

## Build and test on Windows

Requirements: Windows 11 x64, CMake 3.25+, Visual Studio 2022 with C++, and Qt 6.10.3 MSVC 2022 x64 at `.qt/6.10.3/msvc2022_64`.

```powershell
cmake --preset windows-gui-vs2022
cmake --build --preset windows-gui-debug
ctest --preset windows-gui-debug
cmake --build build/windows-gui-vs2022 --config Debug --target jamlink_desktop_qmllint
cmake --build --preset windows-gui-release
ctest --preset windows-gui-release
powershell -ExecutionPolicy Bypass -File scripts/package_windows.ps1
```

## License

JamLink-owned code is [GPL-3.0-or-later](LICENSE). The Windows executable selects Qt and the Steinberg ASIO SDK under GPL version 3. Required notices and corresponding-source details are in [NOTICE](NOTICE), [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md), and [SOURCE_AND_LICENSES.md](SOURCE_AND_LICENSES.md).
