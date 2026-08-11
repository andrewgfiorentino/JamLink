# JamLink two-person test guide

This Windows 11 x64 tester is for exactly two people using direct encrypted UDP audio.

## Both people

1. Extract the entire ZIP to a normal folder. Do not run JamLink from inside the ZIP.
2. Open `JamLink.exe`. The build is not code-signed, so Windows may show a reputation warning. Verify the ZIP hash with the sender before choosing **More info > Run anyway**.
3. If Windows Firewall asks, allow JamLink on the network types you are using.
4. On Sound Check, choose the correct guitar/input, microphone, and headphone/output devices.
   JamLink can use a native ASIO interface for guitar/output and a separate WASAPI USB microphone; no aggregate-device utility is required.
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

- Start quietly. JamLink sends your guitar and your microphone as two separate streams.
- In the Room, each of your friend's streams has its own level slider and switch, so you can turn their guitar down without turning their voice down.
- The Chat button opens private session chat. Chat uses the authenticated room connection and does not pass through a public service.
- The room switch at the bottom mutes or unmutes everything you send to your friend.
- The connection line reports a grade, the measured round trip, and how much receive buffering is currently in use. One-way delay is an estimate derived from the round trip; the round trip and the buffer are measured.
- **Tuner** is the slider icon in the header. Opening it stops your guitar reaching your friend while your microphone keeps working, so you can say "give me a second" and still be heard. Leaving the page always unmutes.
- **Record** writes four separate WAV files: your instrument, your voice, and each of your friend's streams, all aligned to the same timeline. The folder is shown under the button, normally `Music\JamLink\<date time>`. If the card says the disk fell behind, that take has gaps in it.
- Choose **Leave** before changing networks or closing the app.

## Connection troubleshooting

- Both people must run the same build. The wire protocol changed and will not talk to an older JamLink.
- There is no relay server. If the host is behind carrier-grade NAT, symmetric NAT, or a router/firewall that cannot accept a UDP mapping, the direct connection may fail even with a valid invite.
- This test format is uncompressed 48 kHz mono PCM per stream. With guitar and microphone sent separately it uses roughly 1.6 Mbit/s upstream in each direction, about twice the previous combined build. There is no codec yet.
- The receive path now has an adaptive jitter buffer and packet-loss concealment, but neither has been validated against a real Internet path between two homes. Both were measured only against a deterministic impairment model.
- Recording captures what arrives over the network, concealment and all. Pristine local masters exchanged after a take are a later feature.
- Echo cancellation is not enabled. Use headphones.
- This package has passed automated encrypted two-peer socket tests and an automated native Focusrite-ASIO/separate-WASAPI device run. Tonight's session is the first two-home listening validation.

If joining fails, record the exact room status, whether automatic router mapping was ready, the shown UDP port, and whether Windows Firewall permission was accepted.
