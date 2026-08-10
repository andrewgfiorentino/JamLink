# Known limitations

Version 0.2.0 is a friend-test build, not a production release.

- Windows 11 x64 and WASAPI Shared are the only implemented platform/device path. ASIO and WASAPI Exclusive are not implemented.
- The Windows service was automatically opened on development hardware, but device compatibility, dropout behavior, local round-trip latency, and subjective quality have not been validated across hardware.
- Input and remote sample-rate conversion uses a bounded linear interpolator. It handles clock mismatch but has not passed production-quality aliasing/noise benchmarks.
- Network sessions support exactly two peers, IPv4, and direct UDP. There is no signaling service or relay/TURN fallback.
- Automatic router traversal is limited to UPnP same-port mapping plus STUN public-address discovery. Manual UDP forwarding may be required; carrier-grade NAT, symmetric NAT, enterprise firewalls, or blocked UDP can make connection impossible.
- The transport is a custom test protocol using Windows AES-256-GCM. It has not received independent security review, fuzzing, forward-secrecy design, or identity verification.
- Network audio is 48 kHz mono PCM16 at roughly 0.8 Mbit/s upstream per person. Guitar and microphone are mixed together for the friend.
- There is no mature adaptive jitter buffer, reordering window, loss concealment, codec, congestion control, reconnection, or device renegotiation. Packet loss/jitter can cause silence or gaps.
- Displayed network RTT is packet round-trip time, not hardware, acoustic, or end-to-end audio latency.
- Private Soundcheck has real capture, output, local monitoring, meters, and output tone, but no temporary record/playback, tuner, DAW return, or echo cancellation. Use headphones with voice monitoring.
- No recording, tuner, metronome, text chat, plugins, larger rooms, or DAW bridge exists.
- No live-user or cross-home-network validation was performed before the build. All acceptance tests are automated; real-device opening is reported separately and does not close the hardware gate.
- The ZIP is not code-signed. Windows may show a reputation warning.
- The corresponding-source links in `SOURCE_AND_LICENSES.md` are practical for this limited friend test. A durable public release still needs mirrored exact Qt corresponding-source archives and a final deployed-file notice/security inventory.
- The automated `--visual-fixture` uses deterministic fictional device labels and levels only for screenshot/controller tests.
- The official ASIO SDK direction is approved, but no SDK archive or code has been ingested.
