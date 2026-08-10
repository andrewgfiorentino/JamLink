# ADR 0001: Headless, Dependency-Free Core Foundation

- Status: accepted
- Date: 2026-08-10

## Decision

Start JamLink as a C++20 static core library built by CMake, with no runtime third-party dependency. Keep device APIs behind `IAudioDeviceBackend`, represent signal flow as a preallocated bus graph, model each independent device as a clock domain, and validate behavior through deterministic headless tests before adding a GUI or production transport.

Private Soundcheck is a separate local-only processing component whose public interface cannot emit network audio.

## Consequences

- The core can be tested without users, devices, network access, or a GUI.
- Platform, codec, transport, crypto, resampler, and GUI choices remain replaceable.
- Direct Windows audio and asynchronous resampling still need implementation before Phase 1 is complete.
- Framework convenience is intentionally deferred until its license, latency, and real-time behavior can be evaluated against a concrete need.
- The primary repository is GPL-3.0-or-later. Protocol/SDK/IPC boundaries remain separable so a future interoperability component can be explicitly permissively licensed when contributor and dependency rights allow it.
