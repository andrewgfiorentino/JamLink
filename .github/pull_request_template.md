<!-- Copyright (c) 2026 Andrew Fiorentino -->
<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

## What this changes

<!-- What a musician would notice, or what defect this removes. -->

## Why

<!-- What went wrong, or what could not be done before. Evidence beats
     assertion: a log line, a measurement, a failing test. -->

## Licensing

- [ ] I am the copyright holder, **or** I have added my line to
      [CONTRIBUTORS.md](../docs/CONTRIBUTORS.md) agreeing to
      [CLA.md](../docs/CLA.md) in this pull request.

<!-- A contribution merged without this permanently removes the project's
     ability to relicense. It cannot be fixed afterwards. See docs/CONTRIBUTING.md. -->

- [ ] This adds no third-party code, **or** the dependency was agreed first and
      its licence is recorded in docs/THIRD_PARTY_LICENSES.md.

## Checks

- [ ] `ctest --preset windows-debug` passes
- [ ] `ctest --preset windows-gui-debug` passes
- [ ] New behaviour has a test that fails without the change, or the pull
      request explains why it cannot have one
- [ ] Nothing was added to the audio callback that allocates, locks, logs, or
      touches a file or socket
- [ ] No estimate is presented as a measurement
- [ ] No control is shown that cannot act
