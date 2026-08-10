# Desktop Visual QA

<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

## Target and evidence

- Visual source: approved 1536 × 1024 JamLink composite reference supplied for the first GUI. The source image is not committed because its repository redistribution status is not established.
- Primary target: Sound Check panel at source coordinates `(477, 10, 532, 534)`.
- Secondary targets: Settings panel at `(998, 564, 523, 439)` and Home panel at `(13, 11, 446, 530)`.
- Implementation viewport: 532 × 534 logical pixels.
- Windows high-DPI capture: 665 × 668 physical pixels at 125% scale, normalized to 532 × 534 for comparison.
- Automated capture outputs: `build/windows-gui-vs2022/apps/desktop/visual-smoke-{soundcheck,settings,home}.png`.
- Focused comparison outputs: `build/visual-qa/comparison-{soundcheck,settings,home}-side-by-side.png`.

All listed evidence is generated, ignored build output. It is deliberately not source-controlled.

## Implemented-state comparison

### Sound Check

The implementation matches the reference's near-black rounded frame, centered title/privacy statement, paired input cards, uppercase source labels, dark selectors, segmented green/yellow meters, cyan sliders, monitor switches, full-width output card, compact spacing, and purple gradient primary action.

The initial pass exposed dark icon masks, duplicate selector chevrons, too few meter segments, and a flat primary button. The final pass uses pinned Material icon assets with Qt colorization, one explicit selector indicator, 46 meter segments, and a horizontal purple gradient. The post-fix side-by-side comparison shows no P0, P1, or P2 visual defect in the implemented scope.

### Settings

The implementation follows the reference's selected purple Audio sidebar, two-column division, Audio Devices card, aligned device/sample/buffer selectors, separators, status row, rounded frame, and top-right close action. Non-implemented settings categories are muted and labeled `later` so they cannot be mistaken for working navigation.

The reference panel has a different aspect ratio, so it is proportionally scaled and centered on the common comparison canvas. Minor type-size and row-density differences are P3 polish items, not layout or usability failures.

### Home

The implementation carries forward the reference's compact brand header, setup summary card, independent instrument/microphone/output rows, readiness states, and full-width purple Sound Check action. The personalized greeting, recent sessions, and Start/Join actions are replaced with truthful onboarding and an unavailable-room explanation because identity, rooms, invites, and transport do not exist.

## Interaction and state QA

- Home Settings and Check My Sound navigation are wired through the controller.
- Settings close returns Home.
- Automatic launch routing sends missing, unavailable, or stale stored setups to Sound Check and skips to Home only when every stable device/channel/rate/buffer selection resolves.
- Device, sample-rate, and buffer changes invalidate the affected readiness state.
- Home never displays `Ready` after invalidation; it conservatively reports `Check again` until the current setup is verified.
- Sound Check save verifies only the current deterministic fixture configuration and persists it.
- Production mode exposes no synthetic devices/meters and disables backend-dependent controls.
- Monitor gain labels and output dBFS are derived from current values; production mode shows `N/A` rather than fictional measurements.
- First launch resets to safe defaults and writes a versioned preference file.
- Second launch must restore stable device/channel IDs, sample rate, buffer size, monitor gains/toggles, and valid window state.
- Preference loading recovers safe defaults without mutating corrupt input; a later normal application exit deliberately repairs the file with validated defaults.
- Every visible interactive control in the implemented flow has an accessible name.
- Room, tuner, recording, chat, and test-sound controls are absent because their real paths are absent.

## Automated gates

- Debug and Release warnings-as-errors builds pass.
- Debug and Release CTest suites pass all seven registered tests.
- The core executable passes all 22 deterministic cases.
- The typed QML module passes `jamlink_desktop_qmllint` without warnings.
- Three sequential state captures plus a separate exact 150% offscreen-screen-factor capture prove first-launch write, second-launch restore, settings navigation state, minimum logical sizing, and explicit 798 × 801 high-DPI physical sizing without users or audio hardware.

Automated visual fixtures and screenshots do not constitute live-user, real-device, accessibility-assistive-technology, subjective listening, or hardware-latency validation.

## Result

final result: passed
