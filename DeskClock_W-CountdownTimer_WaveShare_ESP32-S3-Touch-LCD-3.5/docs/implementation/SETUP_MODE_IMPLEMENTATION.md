# Setup Mode reboot control

## Physical GPIO0 control

- Short press: switch display screens.
- Hold for at least 3 seconds, then release before 8 seconds: normal reboot.
- Continue holding for 8 seconds: save a one-shot setup request and reboot directly into WiFiManager Setup Mode.

At the 3-second point the display shows:

```text
Release: Reboot
Keep holding for Setup Mode
```

## Web control

The SD-hosted web page includes **System control > Reboot into Setup Mode**.
It sends:

```text
POST /system/setup-mode
```

The firmware saves `forceSetup=true` in the existing `deskclock` Preferences namespace, returns a JSON acknowledgement, and restarts. Early in the next boot, the flag is consumed and removed before `runConfigPortal(true)` is called.

Saved Wi-Fi credentials are not erased. The existing setup portal timeout remains five minutes.
