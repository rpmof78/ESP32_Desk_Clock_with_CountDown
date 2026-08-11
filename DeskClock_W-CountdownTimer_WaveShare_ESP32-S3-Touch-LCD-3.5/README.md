# Desk Countdown Clock — WaveShare ESP32-S3-Touch-LCD-3.5

A Wi-Fi connected desk clock and event countdown built for the **WaveShare ESP32-S3-Touch-LCD-3.5 (SKU 30733)**.

The project combines a landscape 480×320 touch display, NTP-synchronized local time, a calendar-accurate countdown, a Days Remaining view, configurable JPEG backgrounds, battery and USB telemetry, ambient-light dimming, SD-hosted web configuration, event/battery logging, and onboard audio alerts.

Author: **RPMof78**  
Copyright (C) 2026 RPMof78

> **Release status:** release-candidate repository prepared from the clean `VALIDATION1` firmware after successful debug burn-in. Final non-debug burn-in is still in progress before a release tag is assigned.

## Display screens

Screen navigation is circular by touch or a short GPIO0 press:

`Time-of-Day → Countdown → Days Remaining → Time-of-Day`

- **Time-of-Day** — current local time and date. Hours, minutes, and seconds are independently repainted to avoid full-clock flicker.
- **Countdown** — calendar-aware years, months, days, hours, minutes, and seconds until the configured target.
- **Days Remaining** — whole calendar days to the same target, with the target date shown as `dd MMM yyyy` and the same configurable footer as the Countdown screen.

## Major features

- NTP-synchronized local time with configurable primary and secondary NTP servers.
- Configurable POSIX timezone with curated presets in the web UI.
- Calendar-aware event countdown and date-only Days Remaining view.
- Touch and GPIO0 screen navigation.
- SD-card JPEG backgrounds with optional timed rotation.
- Web-based configuration and live status page.
- Editable DHCP/mDNS hostname; default is `countdown.local`.
- DHCP or static IPv4 configuration.
- AXP2101 battery percentage, battery voltage, USB/VBUS state, and onboard TS-thermistor telemetry.
- 15-minute `battery.csv` logging with download and web clear control.
- Rotating `events.log` with download and web clear control.
- Optional external VEML7700 ambient-light sensor for automatic dimming.
- Battery-aware idle dimming.
- ES8311/onboard-speaker System Temperature alert.
- One-time audio celebration when the countdown reaches its target.
- Setup Mode from either the web interface or a long GPIO0 hold.
- Shared-I2C health monitoring and recovery for the AXP2101/VEML7700 path.

## Hardware

Primary target:

- WaveShare ESP32-S3-Touch-LCD-3.5, SKU 30733
- ESP32-S3R8
- 16 MB flash
- 8 MB PSRAM
- ST7796S 320×480 LCD used as 480×320 landscape
- FT6336U capacitive touch
- AXP2101 PMIC
- ES8311 audio codec and onboard speaker
- TCA9554 I/O expander
- onboard TF/microSD interface

Optional external hardware:

- VEML7700 ambient-light sensor on SDA GPIO8 / SCL GPIO7

See [HARDWARE.md](HARDWARE.md) for pin assignments and implementation notes.

## Quick start

1. Install [PlatformIO](https://platformio.org/) with VS Code or PlatformIO Core.
2. Clone or download this repository.
3. Copy the contents of `SD_CARD_CONTENTS/` to the **root** of a FAT-formatted microSD card.
4. Insert the card into the WaveShare board.
5. Build and upload:

```text
pio run -e waveshare-30733 -t upload
```

6. Open the serial monitor at 115200 baud if desired:

```text
pio device monitor -b 115200
```

7. On first setup, use the WiFiManager access point to configure Wi-Fi.
8. Open the device web interface using its IP address or the default mDNS name:

```text
http://countdown.local/
```

See [BUILD.md](BUILD.md) and [OPERATIONS.md](OPERATIONS.md) for details.

## Setup Mode

GPIO0 behavior after boot:

- **Short press:** next display screen.
- **Hold ≥3 seconds, then release before 8 seconds:** normal reboot.
- **Continue holding for 8 seconds:** reboot directly into Setup Mode.

The web interface also provides **Reboot into Setup Mode**.

The firmware currently defines the setup access point as `Counting-Down` with a source-defined default password. Because this repository contains that value in source, treat it as a convenience setup credential, not as a secret.

## SD-card layout

Minimum supplied content:

```text
/
├── audio/
│   ├── countdown_complete.wav
│   ├── system_temp_alert.wav 
├── backgrounds/
│   ├── Your_Background.jpg
│   ├── Anouther_Image.jpg
├── logs/
│   ├── events.log
│   └── battery.csv
├── web/
    ├── index.html
    ├── style.css
    └── app.js
```

The firmware creates and uses `/logs/` at runtime. User JPEG backgrounds are stored under `/backgrounds/`.

### Background images

For best display quality and loading performance, background images should preferably be:

- JPEG / JPG, with baseline JPEG preferred;
- **480 × 320 pixels**;
- landscape orientation with a **3:2 aspect ratio**;
- under **300 KB preferred** and under **500 KB acceptable**;
- moderate in contrast, without bright or highly detailed areas directly behind the clock and countdown text.

Use simple file names such as `Old_Barn.jpg` or `Sunset_Ridge.jpg`. Additional background images are copied to `/backgrounds/` on the microSD card and can then be selected from the web interface.

See [`BACKGROUND_IMAGE_README.md`](BACKGROUND_IMAGE_README.md) for the complete image-preparation guidelines.

See [SD_CARD.md](SD_CARD.md).

## Build environment

The repository uses a single PlatformIO environment:

```text
waveshare-30733
```

Key dependencies are pinned/ranged in `platformio.ini`, including TFT_eSPI, WiFiManager, XPowersLib, TJpg_Decoder, Adafruit BusIO, and Adafruit VEML7700.

## Development status

The debug/instrumented builds used to diagnose web latency have been cleaned out of the validation firmware. The release candidate retains the validated functional fixes but not the temporary latency/request tracing.

The current release gate is the final non-debug burn-in. See [RELEASE_CHECKLIST.md](RELEASE_CHECKLIST.md).

## Third-party material

The bundled DSEG7 font headers are third-party material and retain their original license. See:

- `DSEG7-LICENSE.txt`
- [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)

## License

Copyright (C) 2026 RPMof78

License: **GNU General Public License Version 3 or later**  
SPDX-License-Identifier: `GPL-3.0-or-later`

This software is free software. You may redistribute and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version.

A copy of the GNU GPL v3 license is provided in [`LICENSE.txt`](LICENSE.txt).

Third-party material retains its own license terms. See [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) and `DSEG7-LICENSE.txt`.
