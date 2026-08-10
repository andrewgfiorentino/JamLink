# Known Limitations

The current code is a foundation and cannot be used to jam remotely.

- The native desktop shell is an early source-build preview, not a packaged release. It implements only Home, Private Sound Check, and Audio Settings.
- No ASIO or WASAPI backend is implemented; no real device is enumerated or opened.
- Multiple real device clocks are not aggregated because no resampler is integrated.
- Private Soundcheck has local mixing primitives and a controller/readiness screen, but no real backend connection, output tone, temporary recording/playback, tuner, DAW return, or speaker/mic warning.
- No room, invite, signaling, NAT traversal, relay, transport, codec, chat, encryption, reconnection, or network diagnostics exist.
- No recording, tuner, metronome, text chat, plugin, or DAW bridge exists.
- No measured hardware, local round-trip, LAN, WAN, or end-to-end latency result exists.
- No live-user or physical-device validation was performed; all validation is automated and synthetic.
- Qt 6.10.3 is integrated for source development and automated offscreen QA, but no reviewed deployment bundle or live accessibility/usability validation exists.
- The automated `--visual-fixture` uses deterministic fictional device labels and levels solely for screenshot/controller tests. It is not a production simulation or evidence of hardware support.
- The official ASIO SDK direction is approved but no archive or source has been ingested.
