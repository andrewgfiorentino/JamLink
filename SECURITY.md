# Security

## Status: no Internet feature exists

The current repository opens no sockets and processes no remote input. It does not claim secure sessions, encryption, NAT traversal, or production readiness.

Security gates for the first Internet milestone are:

- use reviewed protocols and libraries; do not design custom cryptography;
- authenticate and encrypt audio, voice, chat, metadata, and control data in transit;
- treat every remote length, count, timestamp, enum, and payload as hostile;
- prevent invite guessing, replay where applicable, downgrade, and secret logging;
- document signaling and relay trust assumptions;
- fuzz packet and control-message parsers;
- keep network and cryptographic work off real-time audio callbacks;
- complete an independent security review before calling Internet sessions production-ready.
