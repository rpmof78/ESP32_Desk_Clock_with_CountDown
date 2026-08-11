# GitHub Release Checklist

## Current gate

- [x] Debug/instrumented burn-in completed successfully.
- [x] Diagnostic code removed from the clean validation firmware.
- [ ] Final non-debug `VALIDATION1` burn-in completed.
- [x] Final event/battery logs reviewed for unexplained faults.
- [ ] Clean build from a fresh clone verified.
- [x] SD card installation tested from the repository's `SD_CARD_CONTENTS` directory.

## Function validation

- [x] Time-of-Day display remains flicker-free.
- [x] Countdown display updates correctly.
- [x] Days Remaining value and `dd MMM yyyy` target date are correct.
- [x] Touch and GPIO0 screen navigation are reliable.
- [x] 3-second reboot and 8-second Setup Mode hold behavior are correct.
- [x] Web page remains responsive during normal operation.
- [x] Background preview/selection/upload/rotation are working.
- [x] Primary/secondary NTP and timezone configuration are working.
- [x] DHCP/static networking and hostname changes are working.
- [x] AXP2101 battery/USB/System Temperature telemetry remains stable.
- [x] Battery discharge and recharge behavior has been validated.
- [x] Event and battery log download/clear controls work.
- [x] VEML7700 auto-dimming works when sensor is installed.
- [x] Speaker temperature alert and countdown-complete celebration work.

## Repository publication

- [x] Project license selected: GPL-3.0-or-later; `LICENSE.txt` added.
- [x] Public author/copyright identity confirmed as RPMof78 / Copyright (C) 2026 RPMof78.
- [ ] Choose the first public version/tag.
- [ ] Update the README release-status paragraph from release candidate to released.
- [ ] Update `CHANGELOG.md` with the selected version and release date.
- [x] Setup Mode AP password confirmed as the intentional public default (`Ten98765432One`).
- [x] Confirm no private Wi-Fi credentials, local IP-specific secrets, logs, or generated build artifacts are committed.
- [ ] Create GitHub release notes from the changelog.
- [ ] Attach only intentionally distributed release artifacts.
