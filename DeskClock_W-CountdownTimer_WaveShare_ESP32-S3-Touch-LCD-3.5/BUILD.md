# Build and Upload

## Requirements

- WaveShare ESP32-S3-Touch-LCD-3.5 (SKU 30733)
- USB connection capable of programming the ESP32-S3
- PlatformIO
- microSD card containing the supplied `SD_CARD_CONTENTS`

## PlatformIO environment

The project contains one environment:

```text
waveshare-30733
```

Build:

```text
pio run -e waveshare-30733
```

Upload:

```text
pio run -e waveshare-30733 -t upload
```

Upload and then monitor:

```text
pio run -e waveshare-30733 -t upload
pio device monitor -b 115200
```

The configured upload speed is 921600 baud and the serial monitor speed is 115200 baud.

## Important TFT configuration

The display depends on compile-time TFT_eSPI settings in `platformio.ini`, including:

- `USE_HSPI_PORT=1`
- `ST7796_DRIVER=1`
- native panel size 320×480
- SPI clock 40 MHz

The firmware then uses `tft.setRotation(1)` for the required 480×320 landscape orientation and `tft.invertDisplay(true)` for correct colors.

Do not replace the TFT_eSPI build flags with a generic user setup unless the equivalent settings are preserved.

## Memory configuration

The project targets the ESP32-S3 N16R8 configuration:

- 16 MB flash
- 8 MB Octal PSRAM
- `qio_opi` Arduino memory type

## SD card

The firmware can boot without the SD card, but web assets, backgrounds, audio files, and persistent SD logs depend on it. For a complete validation, install the supplied card content before testing.

## Development-environment note

Closing or disconnecting the VS Code/PlatformIO serial monitor may reset this ESP32-S3 board because USB serial control-state changes can trigger a reset. During validation, distinguish this development-environment reset from an unexplained firmware reset.
