# DAYS1 - Days Remaining Screen

Adds a third display screen to CLOCKFIX1.

Navigation order:

`Time-of-Day -> Countdown -> Days Remaining -> Time-of-Day`

The Days Remaining screen:

- Uses the same configured countdown target as the Countdown screen.
- Displays whole Gregorian calendar days until the target date.
- Ignores target time-of-day for this date-only view, avoiding DST-related off-by-one errors.
- Displays `0` on the target date and after the target date.
- Uses the same configured Countdown footer text and rendering helper as the Countdown screen.
- Updates the large value only when the calendar-day result changes.
- Uses the current background and battery indicator behavior.
- Leaves WEBFIX4 diagnostics in place for continued testing.

No SD-card web files changed.
