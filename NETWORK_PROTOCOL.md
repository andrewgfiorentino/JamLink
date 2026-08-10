# Network Protocol

## Status: not implemented

No signaling, room protocol, NAT traversal, audio packet format, transport, socket, codec, or network statistic exists in this repository. Private Soundcheck therefore cannot transmit audio.

Before a production protocol is implemented, it must define and automatically test:

- authenticated room and peer establishment;
- encrypted audio, voice, chat, metadata, and control traffic;
- invite entropy and room locking;
- packet versioning, bounds, sequence, timing, replay behavior, and malformed-input handling;
- per-peer jitter, reorder, duplicate, loss, and concealment behavior;
- measured versus estimated latency fields;
- reconnection and stream renegotiation;
- selective subscription for performers and listeners;
- direct connectivity and relay fallback without requiring a permanent commercial service.

Networking must remain behind a JamLink transport abstraction and must never run inside an audio-device callback.
