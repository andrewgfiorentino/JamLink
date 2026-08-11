<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# JamLink directory service

This service provides public profile registration, ephemeral presence, public
lobby discovery, authenticated join metadata, and reports. It never carries
realtime audio or room chat. Established private rooms continue if it stops.

Run locally with Python 3.11 or newer:

```powershell
python jamlink_directory.py --host 127.0.0.1 --port 8787
```

For an Internet deployment, place it behind an HTTPS reverse proxy and keep
its SQLite data volume persistent. The current desktop tester does not expose
public lobbies yet; this service can be exercised through its automated API
tests without affecting private invite rooms.
