# Network protocol

## Scope and stability

`JL1` is an experimental two-peer test protocol. It is not a frozen public interoperability specification and is not production-ready. Current implementation is IPv4, direct UDP, one host, one guest, and no relay.

## Invite

An invite is:

```text
JL1|<public IPv4>|<UDP port>|<64 lowercase hexadecimal characters>
```

The final field is a randomly generated 256-bit bearer secret from Windows CNG. Anyone holding the complete invite can attempt to join while the host is running, so users must share it privately. The invite is not logged or persisted, and stopping the host destroys it.

The host requests a same-number internal/external UDP mapping through Windows UPnP NAT interfaces. Cloudflare's public STUN service at `stun.cloudflare.com:3478` supplies public-address discovery. When mapping is unavailable, JamLink shows the exact local UDP port for manual forwarding. STUN sees the host's public source address; it never receives JamLink audio or the invite secret.

## Packet security

Every datagram has a bounded fixed header containing protocol magic/version, message type, sender nonce prefix, sequence, and payload length. The complete header is AES-GCM additional authenticated data. Payloads use AES-256-GCM through Windows CNG with a per-sender random nonce prefix and monotonically increasing 64-bit packet counter.

The receiver rejects malformed lengths, unknown version/type, authentication failure, replayed/out-of-order sequence at or below the accepted counter, and packets from a different endpoint after pinning. Hello, acknowledgement, ping, pong, and audio messages are all encrypted and authenticated.

This design uses a standard authenticated cipher but is an application protocol that has not received an independent cryptographic or security audit. It does not provide forward secrecy, identity verification beyond possession of the invite, secret rotation, or multi-device account authentication.

## Audio

- mono signed PCM16 little-endian;
- 48,000 frames/second;
- 240 frames per packet (5 ms of audio);
- bounded local and remote SPSC queues;
- network worker performs quantization, encryption, socket I/O, and decryption;
- audio worker performs only bounded queue operations and sample conversion.

The PCM payload is approximately 768 kbit/s in each direction before UDP/IP/authentication overhead. There is no codec, retransmission, forward-error correction, loss concealment, packet reordering window, or mature adaptive jitter buffer. Missing or late data becomes silence.

## State flow

Host: `Preparing -> WaitingForPeer -> Connected`.

Guest: `Connecting -> Connected`.

Either side can report invalid invite, socket failure, encryption failure, or connection loss. The peer endpoint is pinned at successful handshake. Encrypted ping/pong packets update network round-trip time; packet counters and remote peak are displayed separately from any claim of acoustic or end-to-end latency.

## Required hardening

Before production use: independent security review, parser fuzzing, key-agreement/forward-secrecy design, robust NAT traversal and authenticated relay, reconnect/rekey, packet reorder/loss concealment, bandwidth adaptation/codec, congestion behavior, protocol version negotiation, and cross-network impairment testing.
