# JamLink two-person test guide

This is an early Windows 11 x64 Internet test build for exactly two people. It uses direct encrypted UDP audio. Read the limitations below before starting.

## Both people

1. Extract the entire ZIP to a normal folder. Do not run JamLink from inside the ZIP.
2. Open `JamLink.exe`. The build is not code-signed, so Windows may show a reputation warning. Verify the ZIP hash with the sender before choosing **More info > Run anyway**.
3. If Windows Firewall asks, allow JamLink on the network types you are using.
4. On Sound Check, choose the correct guitar/input, microphone, and headphone/output devices.
5. Use headphones. Turn on only the local monitors you need, test the output, then choose **Verify & Save Sound Check**.

## Host

1. From Home, choose **Create Invite**.
2. Copy the full code beginning with `JL1|` and send it privately to your friend. The code contains the room encryption secret. Do not post it publicly.
3. Keep JamLink running while your friend joins.
4. If JamLink says router mapping is ready, no router change should be needed on a typical UPnP-enabled home router.
5. If the friend cannot connect, enable UPnP or forward the exact UDP port shown in JamLink to the host PC, then create and send a new invite.

## Friend

1. Paste the complete invite into the Home screen.
2. Choose **Join**.
3. Wait for the Room screen to say **Connected** before evaluating audio.

## During the test

- Start quietly. JamLink sends the selected guitar and microphone together as mono audio.
- The Room switch mutes or unmutes everything you send to your friend.
- The remote meter, packet counts, and network round-trip time help distinguish silence from a connection problem.
- Choose **Leave** before changing networks or closing the app.

## Important limits

- There is no relay server. If the host is behind carrier-grade NAT, symmetric NAT, or a router/firewall that cannot accept a UDP mapping, the direct connection may fail even with a valid invite.
- This test format is uncompressed 48 kHz mono PCM and uses about 0.8 Mbit/s upstream in each direction.
- There is no packet-loss concealment or adaptive jitter buffer yet. Internet jitter or loss can cause gaps.
- Voice and instrument are combined for transmission. Independent remote controls, chat, recording, tuner, ASIO, echo cancellation, and speaker protection are not implemented.
- No live-user or cross-home-network validation had been performed before this build. The encrypted two-peer path was verified automatically on real loopback sockets, and the Windows device path was automatically opened on development hardware.

If joining fails, record the exact room status, whether automatic router mapping was ready, the shown UDP port, and whether Windows Firewall permission was accepted.
