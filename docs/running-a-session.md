<!-- Copyright (c) 2026 Andrew Fiorentino -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Running a two-person session

A walkthrough for a private JamLink room: two people, direct encrypted audio,
no server in between.

Both people must run the **same version**. The wire protocol is version-checked
and mismatched builds refuse to join rather than failing in a way that looks
like a network problem.

## Both people, once

1. Extract the whole ZIP to a normal folder. Do not run JamLink from inside the
   ZIP.
2. Open `JamLink.exe`. The build is not code-signed, so Windows shows a
   reputation warning. Verify the published SHA-256 against the `.sha256` file
   before choosing **More info → Run anyway**.
3. Allow JamLink through Windows Firewall if prompted. If it was refused
   earlier, Home offers **Fix Firewall**, which requests elevation and adds the
   narrowest useful rule: this executable, inbound, UDP, private and domain
   profiles only.
4. **Use headphones.** There is no echo cancellation.
5. In Sound Check, choose the guitar input, the microphone, and the output.

   **If you have an audio interface, choose its ASIO driver.** ASIO runs
   capture and playback on one clock in one callback; Windows shared audio does
   not, and that difference is audible both in delay and in what the other
   person receives. An ASIO interface for guitar and output can be combined
   with a separate WASAPI USB microphone — no aggregate-device utility needed.

   Buffer size can be left on **Auto**.
6. Click each **Test** control once to confirm it latches red, then click the
   red control to clear it. This check is silent and never enters monitored,
   recorded, or transmitted audio.
7. Play and sing as loudly as you actually will. A red **CLIP** or **CLIP RISK**
   stays visible after the transient. Lower the named hardware input gain, clear
   the latch, and try again. Amber **Hot** is not a failure.
8. Choose **Verify & Save Sound Check** once the latches stay clear.

## Starting a room

**Host:** choose **Start a Jam**, then **Copy Invite**, and send the code
privately. It contains the room encryption secret, so treat it like a password
and send it over a channel you trust.

**Guest:** paste the invite on Home and choose **Join**. Wait for the room to
say **Connected** before judging the audio.

If JamLink reports that your router would not open a port, it will suggest the
other person create the invite instead. That is worth doing rather than working
around: only the host needs to be reachable, so swapping roles fixes it without
touching any router settings.

## During a session

- **Each stream has its own level and mute**, for both people. Your own mutes
  stop that channel reaching your friend while your meters, clipping detection,
  the tuner, and recording all keep running — which is what to use when a vocal
  microphone is picking up guitar bleed.
- **A channel your friend has muted is labelled as muted**, so a deliberate
  choice is never mistaken for a broken connection.
- **Tuner** mutes your guitar to the room by default so you can tune without
  being heard, while your microphone keeps working. Turn that off and it can
  stay open beside the room while you play.
- **Record** writes four aligned WAV files: your instrument, your voice, and
  each of your friend's streams. The folder is shown under the button, normally
  `Music\JamLink\<date time>`. If the card reports the disk fell behind, that
  take has gaps.
- **Audio settings can be changed without leaving the room.** Levels,
  monitoring, and mute apply immediately. Changing an input, output, or buffer
  size restarts the audio device — about a second of silence — but the session
  stays connected and no new invite is needed.
- **The connection line** reports a grade, the measured round trip, and how
  much receive buffering is in use. One-way delay is derived from the round
  trip; the round trip and the buffer are measured.
- Choose **Leave** before changing networks or closing.

## If something goes wrong

The session log is written to `%LOCALAPPDATA%\JamLink\JamLink\`. It records the
audio backend in use, the buffer the device actually settled on, per-stream
packet counts, dropped audio blocks, every port-mapping attempt and its answer,
and how a session ended. The room secret is never written to it and nothing in
it is transmitted anywhere.

`jamlink_net_probe.exe`, included in the package, reports what the network
actually permits: UDP binding, which mapping protocol answered, the public
address, whether the router rewrote the port, and the Windows Firewall state.

Useful details to capture if a connection fails: the exact room status text,
whether automatic router mapping reported ready, the UDP port shown, and
whether Windows Firewall permission was accepted.

## Current limitations

- Two people per room.
- No relay. If both ends sit behind carrier-grade or symmetric NAT, a direct
  connection may be impossible; JamLink reports that rather than retrying
  silently.
- Uncompressed 48 kHz mono PCM per stream, roughly 1.6 Mbit/s upstream with
  guitar and microphone sent separately. There is no codec yet.
- Recording captures what arrived over the network, concealment included.
