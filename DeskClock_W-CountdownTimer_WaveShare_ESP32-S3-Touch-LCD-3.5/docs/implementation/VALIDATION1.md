# VALIDATION1 — Clean Non-Debug Validation Build

Baseline: `DAYS1_DATEFMT1`, after successful debug burn-in.

## Removed for this build

- `COUNTDOWN_DEBUG` latency/timing instrumentation
- `[status]`, `[latency]`, `[web-handler]`, `[web-cycle]`, and `[web-stream]` diagnostic output
- Request-correlation counters and timing structures
- Touch/button diagnostic counters and poll-gap bookkeeping
- Debug-only battery telemetry serial print
- Debug PlatformIO environment and `CORE_DEBUG_LEVEL=4`
- Temporary verbose 404 request tracing

## Retained validated functionality

- Time-of-Day dirty-region clock rendering
- Countdown screen
- Days Remaining screen with `dd MMM yyyy` target date
- Screen order: Time-of-Day -> Countdown -> Days Remaining -> Time-of-Day
- Countdown footer reuse on Days Remaining
- WEBFIX1 response/cache behavior and background preview correction
- Battery/USB/System Temperature telemetry
- 15-minute battery CSV logging and web clear control
- Event logging and web clear control
- AXP2101/I2C recovery behavior
- VEML7700 light sensing and backlight control
- Speaker temperature alert and countdown-complete audio
- Hostname editing and Setup Mode access
- Background selection/rotation and SD-hosted web UI

## Build

Use the single release environment:

```text
pio run -e waveshare-30733 -t upload -t monitor
```

This build is intended for the final non-debug burn-in before release.
