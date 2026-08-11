# Battery Log Clear Control

The Diagnostic logs card now gives `battery.csv` the same download/clear layout and confirmation behavior as `events.log`.

## Endpoint

`POST /logs/battery/clear`

The handler removes the active and rotated battery logs, recreates `/logs/battery.csv` with the current header row, resets the in-memory sample interval state, and records `BATTERY_LOG_CLEARED,source=web` in `events.log`. Battery monitoring remains active and the next telemetry pass can write a fresh baseline sample immediately.
