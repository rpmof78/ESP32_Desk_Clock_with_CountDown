SD Card Contents

Copy the contents of this folder to the root of the microSD card used by the Countdown Clock.

Expected SD card layout:

/
 ├── audio/
 ├── backgrounds/
 ├── logs/
 └── web/

/web/

Contains the files used by the built-in web interface.

Typical contents include:

/web/index.html
/web/style.css
/web/app.js

These files must be present for the normal web dashboard to operate.

/audio/

Contains WAV files used by the speaker subsystem.

Typical contents include:

/audio/system_temp_alert.wav
/audio/countdown_complete.wav

/backgrounds/

Contains JPEG background images used by the display and web background selector.

Additional compatible background images may be added to this folder.

/logs/

Runtime log storage used by the firmware.

Typical files include:

/logs/events.log
/logs/battery.csv

The firmware creates and maintains the log files as needed, so they do not need to be pre-populated before first use.

Notes

Preserve the folder names exactly as shown.

The SD card should be inserted before powering the unit.

Web assets and audio files are supplied in this repository.

Background images may be added or replaced as desired.

Log files are generated and maintained by the firmware.