# CLOCKFIX1

Targeted Time-of-Day screen dirty-region update based on WEBFIX4.

Changes:
- HH, MM, and SS are cached and repainted independently.
- The two colons are static and are drawn only when the clock screen background is drawn.
- Seconds repaint once per second; minutes and hours repaint only when their values change.
- AM/PM and date handling are unchanged.
- Countdown screen behavior is unchanged.
- WEBFIX4 diagnostic instrumentation is retained for ongoing testing and must be removed during final release cleanup.

No SD-card web files changed.
