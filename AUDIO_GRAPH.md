# Audio Graph

## Implemented model

`AudioRouteGraph` is a control-thread-compiled graph of named bus IDs and channel-to-channel routes. Each bus declares a role and channel count. Current roles cover hardware, local and remote music/voice, monitor, cue, network, record, DAW bridge, talkback, click, and hardware output attachment points.

The graph allocates all bus storage during construction. A real-time block then follows three steps:

1. `beginBlock(frames)` clears active preallocated bus regions.
2. Device or worker inputs fill source-bus spans.
3. `processRoutes()` applies routes in a compiled topological order.

Unity routes preserve finite samples and channel identity. One source can feed several destinations without coupling their later gain or mix decisions.

## Private Soundcheck graph boundary

Every graph is compiled for either `PrivateSoundcheck` or `Session`. Private Soundcheck graph construction rejects NetworkSend, NetworkReceive, RemoteMusic, and RemoteVoice buses before processing begins. The current processor has no transport dependency; a future encoded/decoded send preview must remain local, be explicitly labeled, and must not share the live room transport path.

## Not implemented

- dynamic graph swaps while callbacks run;
- feedback routes;
- delay compensation;
- asynchronous resampling nodes;
- platform input/output nodes;
- network, recorder, tuner, or DAW bridge nodes.

Graph construction rejects cycles and orders chained routes before processing begins.
