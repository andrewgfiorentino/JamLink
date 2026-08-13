<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# JamLink directory service

This service provides public profile registration, ephemeral presence, public
lobby discovery, private room-name rendezvous, authenticated join metadata,
and reports. It never carries realtime audio or room chat. Established private
rooms continue if it stops.

Run locally with Python 3.11 or newer:

```powershell
python jamlink_directory.py --host 127.0.0.1 --port 8787
```

For an Internet deployment, place it behind an HTTPS reverse proxy and keep
its SQLite data volume persistent. Start the service with the proxy's exact
source address, for example `--trusted-proxy 127.0.0.1`. The proxy must replace
incoming `X-Forwarded-For` with the real client address; it must not preserve or
append a client-supplied header. Configure the desktop build with
`-DJAMLINK_DIRECTORY_URL=https://directory.example.org`; local development may
instead set `JAMLINK_DIRECTORY_URL=http://127.0.0.1:8787`.

Private room names are unique, uppercase, 4–32 character aliases containing
letters, numbers, and hyphens. They are kept only in memory, expire without a
host heartbeat, never appear in the public lobby list, and do not reveal the
direct encrypted invite until the host admits the waiting request.
