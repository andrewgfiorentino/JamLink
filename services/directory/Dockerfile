# Copyright (c) 2026 Andrew Fiorentino
# SPDX-License-Identifier: GPL-3.0-or-later

FROM python:3.13-slim
WORKDIR /srv/jamlink
COPY jamlink_directory.py .
RUN mkdir -p /srv/jamlink/data && chown -R 65534:65534 /srv/jamlink/data
VOLUME ["/srv/jamlink/data"]
EXPOSE 8787
USER 65534:65534
CMD ["python", "jamlink_directory.py", "--host", "0.0.0.0", "--port", "8787", "--database", "/srv/jamlink/data/directory.sqlite3"]
