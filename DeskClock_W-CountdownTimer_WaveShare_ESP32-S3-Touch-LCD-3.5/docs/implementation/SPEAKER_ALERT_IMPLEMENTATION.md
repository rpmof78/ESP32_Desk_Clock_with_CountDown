# ES8311 System Temperature Alert

## Included behavior

- Plays `/audio/system_temp_alert.wav` through the onboard ES8311 codec and speaker.
- Uses ESP32-S3 I2S pins MCLK 12, BCLK 13, LRCK 15, and DOUT 16.
- Enables the speaker power amplifier through TCA9554 EXIO7.
- Repeats the three-beep sequence every five seconds while the System Temperature alarm remains active.
- Stops playback immediately when the alarm clears, the alert is disabled, or the speaker is muted.
- Stores the enable and mute settings in Preferences.

## Web controls

- **Speaker alert: Enabled/Disabled** controls whether temperature alarms may play audio.
- **Saved mute state: Muted/Unmuted** is stored with the other settings.
- **Mute speaker / Unmute speaker** applies immediately without saving the full settings form.

## SD card installation

Copy the contents of `SD_CARD_CONTENTS` to the SD card root so these paths exist:

- `/web/index.html`
- `/web/style.css`
- `/web/app.js`
- `/audio/system_temp_alert.wav`

The WAV must remain mono, 16-bit PCM, 16 kHz. The supplied file meets those requirements.

## First validation

After upload, look for a serial line similar to:

```
[speaker] codec=1 wav=1 path=/audio/system_temp_alert.wav
```

The web page should report `Armed / OK`. Use a temporarily reduced alarm threshold for a controlled sound test, then restore the intended threshold.


## Countdown completion celebration

- File: `/audio/countdown_complete.wav`
- Format: mono, signed 16-bit PCM, 16 kHz
- Plays once on the transition from an active countdown to `TARGET REACHED`.
- Does not replay merely because the unit boots with an already-expired target.
- Uses the same Speaker enabled/disabled and mute controls as the temperature alert.
- The celebration takes priority over a temperature-alert sequence; thermal alert repetition resumes afterward if still active.
