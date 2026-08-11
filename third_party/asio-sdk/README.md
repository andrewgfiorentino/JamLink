<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Steinberg ASIO SDK subset

JamLink vendors an unmodified build subset of the official Steinberg ASIO SDK
2.3.4 archive, `ASIO-SDK_2.3.4_2025-10-15.zip`:

- official download: <https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip>
- SHA-256: `d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca`
- selected license for the interface files: GPL version 3
- helper-file license: the embedded three-clause BSD-style license in each helper file

The retained files are `common/asio.cpp`, `common/asio.h`,
`common/asiosys.h`, `common/iasiodrv.h`, `host/asiodrivers.cpp`,
`host/asiodrivers.h`, `host/ginclude.h`, `host/pc/asiolist.cpp`, and
`host/pc/asiolist.h`. The top-level SDK license is retained as `LICENSE.txt`.
Hardware-vendor drivers are not part of the SDK and are not redistributed.

ASIO is a trademark of Steinberg Media Technologies GmbH. JamLink uses the
plain-text name only to identify the compatible audio backend and installed
drivers; it does not use an ASIO logo.
