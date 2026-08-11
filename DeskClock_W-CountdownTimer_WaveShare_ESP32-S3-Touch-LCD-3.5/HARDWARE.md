# Hardware and Pin Assignments

Target board: **WaveShare ESP32-S3-Touch-LCD-3.5 (SKU 30733)**.

## Application pin assignments

| Function | Pin / interface | Notes |
|---|---:|---|
| TFT MOSI | GPIO1 | TFT_eSPI |
| TFT MISO | GPIO2 | TFT_eSPI |
| TFT SCLK | GPIO5 | TFT_eSPI |
| TFT DC | GPIO3 | TFT_eSPI |
| TFT backlight PWM | GPIO6 | 5 kHz, 8-bit PWM |
| Shared I2C SDA | GPIO8 | AXP2101, FT6336U, TCA9554, ES8311, optional VEML7700 |
| Shared I2C SCL | GPIO7 | 100 kHz application bus |
| SD_MMC D0 | GPIO9 | 1-bit SD_MMC |
| SD_MMC CMD | GPIO10 | 1-bit SD_MMC |
| SD_MMC CLK | GPIO11 | 1-bit SD_MMC |
| I2S MCLK | GPIO12 | ES8311 speaker audio |
| I2S BCLK | GPIO13 | ES8311 speaker audio |
| I2S LRCK | GPIO15 | ES8311 speaker audio |
| I2S DOUT | GPIO16 | ES8311 speaker audio |
| Application button | GPIO0 | onboard BOOT button after startup |

## Shared I2C devices

| Device | Address | Purpose |
|---|---:|---|
| AXP2101 | library-defined AXP2101 address | battery, VBUS, TS thermistor |
| ES8311 | `0x18` | audio codec |
| TCA9554 | `0x20` | LCD reset and speaker amplifier control |
| FT6336U | `0x38` | capacitive touch |
| VEML7700 | `0x10` | optional ambient-light sensor |

The firmware initializes the AXP2101 before binding the VEML7700 to the shared `Wire` instance because XPowersLib initializes/reconfigures the bus.

## TCA9554 use

- EXIO1 / bit 1: LCD reset
- EXIO7 / bit 7: speaker power amplifier enable

## Display

- Controller: ST7796S
- Native geometry: 320×480
- Application geometry: 480×320 landscape
- TFT_eSPI rotation: `1`
- Color inversion: enabled with `tft.invertDisplay(true)`

## Touch

FT6336U native portrait coordinates are mapped into the rotated 480×320 landscape display.

## AXP2101 System Temperature

The board-mounted 10 kΩ NTC connected to the AXP2101 TS input is treated as **System Temperature**. It is not identified by this project as a battery-pack temperature sensor.

The temperature alert is an early-warning function, not a safety cutoff.

## Optional VEML7700

The external VEML7700 connects to the shared I2C bus:

- VCC: 3.3 V as appropriate for the module used
- GND: GND
- SDA: GPIO8
- SCL: GPIO7

If the VEML7700 is absent, the firmware continues operating and falls back to the remaining dimming behavior.

## Onboard RGB status LED note

The board documentation associates GPIO47 with the onboard RGB status LED, but direct sustained GPIO47 HIGH/LOW testing in this project produced no visible control of the LED. The project therefore does not use GPIO47 for application status indication.
