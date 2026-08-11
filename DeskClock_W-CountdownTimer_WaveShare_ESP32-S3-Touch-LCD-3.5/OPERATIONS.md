# Operation and Configuration

## Screen navigation

Touch the screen or short-press GPIO0 to move through:

```text
Time-of-Day → Countdown → Days Remaining → Time-of-Day
```

## GPIO0 long-press behavior

- Short press: next screen
- Hold at least 3 seconds and release before 8 seconds: reboot
- Continue holding to 8 seconds: reboot into Setup Mode

At the 3-second point the display prompts the operator to release for a reboot or continue holding for Setup Mode.

## Web interface

The web UI provides live status and configuration for the major runtime features, including:

- countdown target and footer text
- local timezone
- primary and secondary NTP servers
- network hostname
- DHCP/static IPv4 settings
- background selection/upload/delete/rotation
- backlight and ambient-light thresholds
- System Temperature alert thresholds
- speaker enabled/muted state
- log download/clear controls
- reboot into Setup Mode

The default hostname is:

```text
countdown.local
```

## Hostname rules

The hostname editor accepts:

- 1–32 characters
- letters, numbers, and hyphens
- no leading or trailing hyphen

Uppercase input is normalized to lowercase. Enter the hostname only; `.local` is appended for display/use.

## Setup Mode

The setup portal does not erase saved Wi-Fi credentials simply by entering Setup Mode. It provides a controlled path to change network configuration.

## Backlight behavior

Backlight behavior combines:

- USB/VBUS versus battery power state
- idle timeout
- optional VEML7700 ambient-light thresholds

Lux dim/undim thresholds use hysteresis to avoid rapid brightness chatter near the threshold.

## System Temperature alert

The onboard AXP2101 TS thermistor is presented as **System Temperature**.

The supplied defaults are conservative early-warning thresholds. When alarming and audio is enabled/unmuted, the alert WAV repeats approximately every five seconds. The countdown-complete celebration has priority and plays once on the transition to the reached state.

## Battery logging

Battery telemetry is written at 15-minute intervals. The web clear control is useful before a dedicated charge/discharge test because it starts a clean CSV dataset without disabling monitoring.
