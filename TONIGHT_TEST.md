# JamLink two-person test guide

This Windows 11 x64 tester is for exactly two people using direct encrypted UDP audio.
The 0.3.4 tester uses the infrastructure-independent **Copy Invite** flow.
Temporary custom short codes are not enabled in this package because no shared
rendezvous endpoint is configured; their absence does not affect direct audio,
chat, recording, or connection security.

## Both people

1. Extract the entire ZIP to a normal folder. Do not run JamLink from inside the ZIP.
2. Open `JamLink.exe`. The build is not code-signed, so Windows may show a reputation warning. Verify the ZIP hash with the sender before choosing **More info > Run anyway**.
3. If Windows Firewall asks, allow JamLink on the network types you are using.
4. On Sound Check, choose the correct guitar/input, microphone, and headphone/output devices.
   JamLink can use a native ASIO interface for guitar/output and a separate WASAPI USB microphone; no aggregate-device utility is required.
5. Use headphones. Turn on only the local monitors you need and test the output.
6. Click each clean **Test** control once to verify that it turns into a red **CLIP** latch, then click the red control to reset it. This test is silent and does not enter monitored, recorded, or network audio.
7. Play and sing as loudly as you realistically expect tonight. A red **CLIP** or **CLIP RISK** warning stays visible after the transient. Lower the named hardware input gain, then press the red indicator to reset and try again. Amber **Hot** is not a failure.
8. Choose **Verify & Save Sound Check** only after the clipping latches stay clear.

## Host

1. From Home, choose **Start a Jam**. If the temporary-code panel appears, enter a memorable 4-64 character code or choose **Generate Random**, then choose **Create**.
2. Copy the short code and send it to your friend. It expires with this jam and is not listed publicly.
3. When your friend's request appears, confirm their displayed name and choose **Let In**. No room audio is available to them before this approval.
4. If temporary codes are unavailable, JamLink creates a full code beginning with `JL1|` instead. Send that code privately; it contains the room encryption secret and does not use the waiting room.
5. Keep JamLink running while your friend joins.
6. If JamLink says router mapping is ready, no router change should be needed on a typical UPnP-enabled home router.
7. If the friend cannot connect, enable UPnP or forward the exact UDP port shown in JamLink to the host PC, then create and send a new invite.

## Friend

1. Enter the temporary code or paste the complete `JL1|` invite into the Home screen.
2. Choose **Join**.
3. For a temporary code, wait for the host to approve the request. Then wait for the Room screen to say **Connected** before evaluating audio.

## During the test

- Start quietly. JamLink sends your guitar and your microphone as two separate streams.
- Your guitar, voice, and monitor-mix clip indicators remain latched in the Room until you reset them, so a short overload cannot disappear unnoticed.
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
