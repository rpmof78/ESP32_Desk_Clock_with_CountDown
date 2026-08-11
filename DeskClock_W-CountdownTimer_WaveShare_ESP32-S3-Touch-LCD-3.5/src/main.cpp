/*
 * Desk Countdown Clock
 * --------------------
 * Copyright (C) 2026 RPMof78
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See LICENSE.txt.
 *
 * Target hardware: WaveShare 30733 ESP32-S3-Touch-LCD-3.5.
 *
 * Onboard hardware used by this application:
 *   - ESP32-S3R8 with 16 MB flash and 8 MB PSRAM
 *   - ST7796S 320x480 LCD, operated as 480x320 landscape
 *   - FT6336U capacitive touch controller on the shared I2C bus
 *   - AXP2101 PMIC for battery, VBUS, and onboard TS-thermistor monitoring
 *   - TCA9554 I/O expander for LCD reset
 *   - 1-bit SD_MMC interface for the onboard TF card
 *   - GPIO0 BOOT button as the application button after startup
 *
 * External hardware retained:
 *   - VEML7700 ambient-light sensor on I2C SDA GPIO8 / SCL GPIO7
 *
 * Main functions:
 *   - Calendar-accurate countdown and current date/time screens
 *   - Touch or GPIO0 screen switching
 *   - Hold GPIO0 for 3 seconds, then release, for a normal reboot
 *   - Continue holding GPIO0 for 8 seconds to reboot directly into Setup Mode
 *   - Wi-Fi setup, NTP synchronization, configurable timezone and NTP servers
 *   - SD-card JPEG backgrounds with PSRAM buffering
 *   - Automatic backlight control from VBUS/battery state, idle time, and lux
 *   - AXP2101 battery voltage, charge percentage, and power-state reporting
 *   - System Temperature from the onboard 10 kOhm NTC connected to AXP2101 TS
 *   - Web configuration, live status, event logging, and battery telemetry logging
 *
 * Display configuration:
 *   - TFT_eSPI uses HSPI; platformio.ini must define USE_HSPI_PORT=1.
 *   - Native panel dimensions remain 320x480 in TFT_eSPI configuration.
 *   - tft.setRotation(1) provides the required 180-degree landscape orientation.
 *   - tft.invertDisplay(true) is required for correct ST7796S colors.
 *
 * Temperature note:
 *   The AXP2101 TS input is connected to a PCB-mounted NTC, not to a dedicated
 *   battery-pack sensor. The application therefore identifies it as System
 *   Temperature. It is suitable for compact-enclosure trend monitoring and
 *   high-temperature warning after threshold validation on the assembled unit.
 *
 * Audio:
 *   - ES8311 codec and onboard speaker driven through ESP32-S3 I2S
 *   - Temperature alert WAV: /audio/system_temp_alert.wav
 *   - One-time countdown celebration WAV: /audio/countdown_complete.wav
 *   - Web-configurable enable/disable and persistent mute controls
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>       // tzapu/WiFiManager
#include <Preferences.h>
#include <TFT_eSPI.h>
#define GFXFF 1 // TFT_eSPI expects the sketch to define this itself for free-font drawString() calls
#include "DSEG7_Classic_Bold_40pt7b.h" // 7-segment font for the clock screen's time (see DSEG7-LICENSE.txt)
#include "DSEG7_Classic_Bold_24pt7b.h" // smaller variant for the COUNTDOWN screen's 2x3 grid
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>       // onboard AXP2101 PMU
#include <Adafruit_VEML7700.h> // ambient light sensor for auto-dimming
#include <SPI.h>
#include <SD_MMC.h>
#include <TJpg_Decoder.h>      // bodmer/TJpg_Decoder
#include <driver/i2s.h>
#include <vector>
#include <time.h>

// ---------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------
#define BUTTON_PIN     0   // onboard BOOT button; application input after boot

// Setup-mode access point
#define AP_NAME        "Counting-Down"
#define AP_PASS        "Ten98765432One"
IPAddress AP_IP(192, 168, 10, 10);
IPAddress AP_GATEWAY(192, 168, 10, 10);
IPAddress AP_SUBNET(255, 255, 255, 0);

#define DEFAULT_NTP_SERVER_1   "pool.ntp.org"
#define DEFAULT_NTP_SERVER_2   "time.nist.gov"
#define DEFAULT_TZ     "EST5EDT,M3.2.0,M11.1.0"   // US Eastern w/ DST, change as needed

// Curated timezone presets for the live web page's dropdown -- POSIX TZ
// strings pulled from https://github.com/nayarsystems/posix_tz_db to avoid
// hand-transcribing DST transition rules incorrectly. Each of these already
// includes the correct DST rule where applicable (that's what the second
// clause after the comma encodes) -- daylight saving was always handled
// automatically once a correct string was in the field; a hand-typed
// string missing that clause is what silently loses automatic DST.
struct TzPreset { const char* label; const char* tz; };
const TzPreset TZ_PRESETS[] = {
  {"US Eastern (New York)",                    "EST5EDT,M3.2.0,M11.1.0"},
  {"US Central (Chicago)",                     "CST6CDT,M3.2.0,M11.1.0"},
  {"US Mountain (Denver)",                      "MST7MDT,M3.2.0,M11.1.0"},
  {"US Mountain, no DST (Phoenix)",             "MST7"},
  {"US Pacific (Los Angeles)",                  "PST8PDT,M3.2.0,M11.1.0"},
  {"US Alaska (Anchorage)",                     "AKST9AKDT,M3.2.0,M11.1.0"},
  {"US Hawaii (Honolulu)",                      "HST10"},
  {"Mexico City",                               "CST6"},
  {"Brazil (Sao Paulo)",                        "<-03>3"},
  {"UK (London)",                               "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Central Europe (Paris/Berlin/Madrid/Rome)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Eastern Europe (Athens)",                   "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Russia (Moscow)",                           "MSK-3"},
  {"Egypt (Cairo)",                             "EET-2EEST,M4.5.5/0,M10.5.4/24"},
  {"South Africa (Johannesburg)",               "SAST-2"},
  {"UAE (Dubai)",                               "<+04>-4"},
  {"India (Kolkata)",                           "IST-5:30"},
  {"Pakistan (Karachi)",                        "PKT-5"},
  {"Bangladesh (Dhaka)",                        "<+06>-6"},
  {"Thailand (Bangkok)",                        "<+07>-7"},
  {"China (Shanghai)",                          "CST-8"},
  {"Hong Kong",                                 "HKT-8"},
  {"Singapore",                                 "<+08>-8"},
  {"Japan (Tokyo)",                             "JST-9"},
  {"South Korea (Seoul)",                       "KST-9"},
  {"Australia Eastern (Sydney)",                "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"Australia Central (Adelaide)",              "ACST-9:30ACDT,M10.1.0,M4.1.0/3"},
  {"Australia Western (Perth)",                 "AWST-8"},
  {"New Zealand (Auckland)",                    "NZST-12NZDT,M9.5.0,M4.1.0/3"},
  {"UTC",                                       "UTC0"},
};
const int TZ_PRESET_COUNT = sizeof(TZ_PRESETS) / sizeof(TZ_PRESETS[0]);
#define DEFAULT_HOSTNAME "countdown"              // reachable at http://countdown.local/
#define HOSTNAME_MAX_LENGTH 32

// ---- Backlight / dimming ----
#define BACKLIGHT_PIN       6   // onboard LCD backlight
#define BACKLIGHT_PWM_FREQ  5000
#define BACKLIGHT_PWM_RES   8      // 8-bit -> 0-255
#define BACKLIGHT_PWM_CHANNEL 0

// ---- Onboard RGB status LED (GPIO47) -- NOT software-controllable ----
// This board's documentation labels GPIO47 as the RGB status LED pin, and
// it ships lit bright green out of the box. A definitive test (sustained
// plain digitalWrite HIGH, then LOW, for 10 seconds each, no WS2812
// protocol involved at all) produced zero visible change -- meaning this
// pin does not control the LED's brightness/state via any software
// signal, contrary to what the documentation implies. Since a bit-banged
// WS2812 waveform is just rapid HIGH/LOW pulses on the same pin, this also
// rules out every WS2812-specific theory (timing, protocol variant,
// PSRAM-adjacent interference) that earlier attempts were built around --
// none of that matters if the pin isn't in the LED's control path at all.
// There is no firmware fix for this. If the light leak matters (e.g. for
// an enclosed project box), the practical fix is physical: a small piece
// of opaque tape, heat-shrink, or blackout paint over the LED itself.
constexpr unsigned long USB_READ_INTERVAL_MS = 250; // AXP2101 polling interval

#define DEFAULT_DIM_PERCENT   20    // brightness % while dimmed
#define DEFAULT_IDLE_TIMEOUT  30    // seconds of no touch before dimming (on battery only)

// ---- Ambient light sensor (VEML7700) auto-dimming ----
// Optional -- if not present at boot, this is skipped entirely and dimming
// falls back to just the existing battery+idle-timeout behavior below.
#define DEFAULT_DIM_BELOW_LUX     20.0  // dim when ambient light drops below this
#define DEFAULT_UNDIM_ABOVE_LUX   40.0  // undim once it rises back above this (hysteresis gap vs the above)
#define LUX_READ_INTERVAL_MS      2000  // ambient light changes slowly -- no need to poll faster
Adafruit_VEML7700 veml;
bool lightSensorFound = false;
float currentLux = -1;
float dimBelowLux = DEFAULT_DIM_BELOW_LUX;
float undimAboveLux = DEFAULT_UNDIM_ABOVE_LUX;
bool ambientDark = false;         // hysteresis state, like usbPresent below
unsigned long lastLuxReadMs = 0;

// ---- System high-temperature alert state ----
// Early-warning only, not a safety cutoff -- see the earlier design
// discussion on why an automated power cutoff can't stop genuine thermal
// runaway, and why a low, conservative trip point that gets a person
// involved is the more useful layer here. Threshold trips well below
// dangerous (default 45C) so there's time to act; hysteresis (separate
// trip/clear thresholds) avoids rapid on/off chatter right at the edge.
// Pulses on/off while alarming rather than a continuous tone -- easier on
// the ears and more likely to draw attention over time.
#define DEFAULT_BUZZER_TRIP_C   45.0
#define DEFAULT_BUZZER_CLEAR_C  40.0
bool buzzerEnabled = true;      // retained internal preference name for compatibility
float buzzerTripC = DEFAULT_BUZZER_TRIP_C;
float buzzerClearC = DEFAULT_BUZZER_CLEAR_C;
bool buzzerAlarming = false;    // thermal hysteresis state
bool speakerMuted = false;      // persistent operator mute, independent of enable
bool speakerCodecReady = false;
bool speakerPlaying = false;
bool speakerFileAvailable = false;
bool celebrationFileAvailable = false;
bool celebrationPending = false;
bool countdownReachedStateKnown = false;
bool countdownWasReached = false;
unsigned long lastSpeakerSequenceMs = 0;

enum class SpeakerPlaybackKind : uint8_t {
  None,
  TemperatureAlert,
  CountdownCelebration
};
SpeakerPlaybackKind speakerPlaybackKind = SpeakerPlaybackKind::None;

constexpr char SPEAKER_ALERT_WAV_PATH[] = "/audio/system_temp_alert.wav";
constexpr char COUNTDOWN_CELEBRATION_WAV_PATH[] = "/audio/countdown_complete.wav";
constexpr unsigned long SPEAKER_REPEAT_INTERVAL_MS = 5000;
constexpr uint8_t ES8311_I2C_ADDRESS = 0x18;
constexpr i2s_port_t SPEAKER_I2S_PORT = I2S_NUM_0;
constexpr int SPEAKER_I2S_MCLK = 12;
constexpr int SPEAKER_I2S_BCLK = 13;
constexpr int SPEAKER_I2S_LRCK = 15;
constexpr int SPEAKER_I2S_DOUT = 16;
constexpr uint32_t SPEAKER_SAMPLE_RATE = 16000;
constexpr uint32_t SPEAKER_FIXED_MCLK = SPEAKER_SAMPLE_RATE * 256UL;

File speakerFile;
uint32_t speakerDataRemaining = 0;

// ---- Fuel gauge (AXP2101) ----
#define I2C_SDA_PIN          8
#define I2C_SCL_PIN          7
#define BATTERY_READ_MS      5000   // how often to poll the gauge over I2C

// ---- Shared I2C bus health / recovery ----
constexpr uint32_t I2C_CLOCK_HZ = 100000;
constexpr uint32_t I2C_TIMEOUT_MS = 50;
constexpr uint8_t I2C_FAILURES_BEFORE_RECOVERY = 3;
constexpr unsigned long I2C_RECOVERY_FAST_MS = 5000;
constexpr unsigned long I2C_RECOVERY_MEDIUM_MS = 30000;
constexpr unsigned long I2C_RECOVERY_SLOW_MS = 300000;
constexpr uint8_t I2C_RECOVERY_FAST_FAILURES = 3;
constexpr uint8_t I2C_RECOVERY_MEDIUM_FAILURES = 10;
constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 15000;
constexpr uint8_t AXP2101_I2C_ADDRESS = AXP2101_SLAVE_ADDRESS;
constexpr uint8_t FT6336_I2C_ADDRESS = 0x38;
constexpr uint8_t VEML7700_I2C_ADDRESS = 0x10;

// ---- Onboard TF card, 1-bit SD_MMC mode ----
#define SD_CLK_PIN            11
#define SD_CMD_PIN            10
#define SD_D0_PIN              9
#define BACKGROUNDS_DIR       "/backgrounds"
#define LOGS_DIR              "/logs"
#define I2C_EVENT_LOG_PATH    "/logs/events.log"
#define BATTERY_LOG_PATH      "/logs/battery.csv"
#define BATTERY_LOG_OLD_PATH  "/logs/battery.1.csv"
#define WEB_ROOT_DIR          "/web"
#define WEB_INDEX_PATH        "/web/index.html"
#define I2C_EVENT_LOG_MAX     (256UL * 1024UL)
#define BATTERY_LOG_MAX       (512UL * 1024UL)
constexpr unsigned long BATTERY_LOG_INTERVAL_MS = 15UL * 60UL * 1000UL;
#define SCREEN_W              480
#define SCREEN_H               320

#define SD SD_MMC  // preserve existing filesystem call sites

TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
WiFiManager wm;
WebServer server(80);
XPowersPMU power;

bool shouldSaveConfig = false;

struct TargetDate {
  int year, month, day, hour, minute, second;
};
TargetDate target;
char tzString[64];
char ntpServer1[64];
char ntpServer2[64];
char networkHostname[HOSTNAME_MAX_LENGTH + 1] = DEFAULT_HOSTNAME;
char countdownEventText[48] = "TO GO..!!..";
bool networkDhcp = true;
IPAddress networkIp(192, 168, 1, 50);
IPAddress networkGateway(192, 168, 1, 1);
IPAddress networkSubnet(255, 255, 255, 0);
IPAddress networkDns1(1, 1, 1, 1);
IPAddress networkDns2(8, 8, 8, 8);

int dimPercent    = DEFAULT_DIM_PERCENT;
int idleTimeoutSec = DEFAULT_IDLE_TIMEOUT;
bool use12HourTime = false; // false = 24-hour (default), true = 12-hour w/ AM|PM indicator

// WiFiManager custom-field value buffers (must persist for the portal's lifetime)
char p_year[6], p_month[4], p_day[4], p_hour[4], p_minute[4], p_second[4], p_tz[64];
char p_dim[4], p_idle[5];

enum Screen { SCREEN_CLOCK, SCREEN_COUNTDOWN, SCREEN_DAYS_REMAINING };
Screen currentScreen = SCREEN_COUNTDOWN;

// Cached strings so we only repaint digits that actually changed (no flicker)
// Countdown grid: 2 rows x 3 columns, one independently-positioned value per
// cell (all Font 7) with its label locked to the same x-center -- guarantees
// alignment regardless of digit count, unlike drawing one combined string.
String lastCdVal[6] = {"", "", "", "", "", ""}; // Y, Mo, D, H, Mi, S
bool lastCdReached = false;
String lastCdTargetStr = ""; // target date/time line, just below the title
String lastClockHour = "", lastClockMinute = "", lastClockSecond = "";
String lastClockLine2 = "";
String lastAmPmStr = ""; // "" when 24-hour mode (indicator hidden), "AM"/"PM" when 12-hour
String lastDaysRemainingValue = "";
String lastDaysRemainingTarget = "";

unsigned long lastTouchMs = 0;
const unsigned long TOUCH_DEBOUNCE_MS = 400;

unsigned long lastActivityMs = 0;
bool usbPresent = false;
bool usbPresenceInitialized = false;
uint8_t usbPresentConfirmCount = 0;
uint8_t usbAbsentConfirmCount = 0;
unsigned long lastUsbReadMs = 0;
float inputRailVoltage = -1; // reconstructed voltage on the UPS board's raw USB input
bool backlightDimmed = false;

bool pmuFound = false;
bool batteryFound = false;
float batteryPercent = -1;
float batteryVoltage = -1;
float systemTempC = NAN; // onboard PCB NTC through the AXP2101 TS input
bool systemTempValid = false;
bool systemTempFault = false;
uint8_t systemTempValidSamples = 0;
uint8_t systemTempFaultSamples = 0;
unsigned long lastBatteryReadMs = 0;
unsigned long lastBatteryLogMs = 0;
bool batteryLogHasSample = false;
String lastBatteryText = "";

bool i2cBusReady = false;
bool i2cRecoveryRequested = false;
bool batteryI2cExpected = false;
bool lightI2cExpected = false;
uint8_t batteryI2cFailures = 0;
uint8_t lightI2cFailures = 0;
unsigned long lastI2cRecoveryMs = 0;
uint32_t i2cRecoveryCount = 0;
uint16_t i2cRecoveryFailureStreak = 0;
unsigned long lastWifiReconnectAttemptMs = 0;


// ---- Background image state ----
bool sdReady = false;
uint16_t *bgBuffer = nullptr;       // PSRAM: SCREEN_W * SCREEN_H, RGB565
String currentBgFile = "";          // "" = no background (solid black)
bool bgLoadFailed = false;

// ---- Background auto-rotation ----
#define DEFAULT_BG_ROTATION_MIN  15
#define MIN_BG_ROTATION_MIN       5   // hard floor, enforced server-side regardless of what's posted
bool bgRotationEnabled = false;
int bgRotationIntervalMin = DEFAULT_BG_ROTATION_MIN;
bool bgRotationShuffle = false;      // false = sequential, true = shuffle
int bgRotationIndex = -1;            // rotation's own position -- independent of manual picks, so a
                                      // manual selection gets overridden at the next interval rather
                                      // than derailing the sequence/shuffle state
unsigned long lastBgRotationMs = 0;


// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------
void saveConfigCallback() { shouldSaveConfig = true; }

bool isLeapYear(int y) {
  return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

const char* monthAbbrev(int month) { // 1-12
  static const char* names[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
  if (month < 1 || month > 12) return "???";
  return names[month - 1];
}

const char* monthFullName(int month) { // 1-12
  static const char* names[] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
  };
  if (month < 1 || month > 12) return "Unknown";
  return names[month - 1];
}

// Gregorian calendar ordinal used for the Days Remaining screen.  This is
// intentionally date-only arithmetic, so DST changes and the configured
// target time-of-day cannot create a 23/25-hour off-by-one error.
int32_t calendarDayOrdinal(int year, int month, int day) {
  static const int daysBeforeMonth[] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
  };
  const int32_t y = year - 1;
  int32_t ordinal = y * 365L + y / 4 - y / 100 + y / 400;
  ordinal += daysBeforeMonth[month - 1] + day;
  if (month > 2 && isLeapYear(year)) ordinal++;
  return ordinal;
}

int daysRemainingToTargetDate(const struct tm &now, const TargetDate &tgt) {
  const int32_t today = calendarDayOrdinal(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
  const int32_t targetDay = calendarDayOrdinal(tgt.year, tgt.month, tgt.day);
  const int32_t remaining = targetDay - today;
  return remaining > 0 ? (int)remaining : 0;
}

int daysInMonth(int month, int year) {
  static const int dim[] = {31,28,31,30,31,30,31,31,30,31,30,31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return dim[month - 1];
}

TargetDate sanitizeTargetDate(TargetDate t) {
  t.year = constrain(t.year, 2024, 2100);
  t.month = constrain(t.month, 1, 12);
  const int maxDay = daysInMonth(t.month, t.year);
  t.day = constrain(t.day, 1, maxDay);
  t.hour = constrain(t.hour, 0, 23);
  t.minute = constrain(t.minute, 0, 59);
  t.second = constrain(t.second, 0, 59);
  return t;
}

struct CountdownParts {
  int years, months, days, hours, minutes, seconds;
  bool reached;
};

// Calendar-accurate difference (like calculating someone's age) rather than
// a flat seconds/86400-style breakdown.
CountdownParts calendarDiff(struct tm &now, TargetDate &tgt) {
  CountdownParts out = {0,0,0,0,0,0,false};

  int y1 = now.tm_year + 1900, mo1 = now.tm_mon + 1, d1 = now.tm_mday;
  int h1 = now.tm_hour, mi1 = now.tm_min, s1 = now.tm_sec;
  int y2 = tgt.year, mo2 = tgt.month, d2 = tgt.day;
  int h2 = tgt.hour, mi2 = tgt.minute, s2 = tgt.second;

  if (y1 > y2 || (y1 == y2 && mo1 > mo2) ||
      (y1 == y2 && mo1 == mo2 && d1 > d2) ||
      (y1 == y2 && mo1 == mo2 && d1 == d2 && h1 > h2) ||
      (y1 == y2 && mo1 == mo2 && d1 == d2 && h1 == h2 && mi1 > mi2) ||
      (y1 == y2 && mo1 == mo2 && d1 == d2 && h1 == h2 && mi1 == mi2 && s1 >= s2)) {
    out.reached = true;
    return out;
  }

  int seconds = s2 - s1, minutes = mi2 - mi1, hours = h2 - h1;
  int days = d2 - d1, months = mo2 - mo1, years = y2 - y1;

  if (seconds < 0) { seconds += 60; minutes--; }
  if (minutes < 0) { minutes += 60; hours--; }
  if (hours   < 0) { hours   += 24; days--; }
  int borrowMonth = mo2;
  int borrowYear = y2;
  while (days < 0) {
    borrowMonth--;
    if (borrowMonth == 0) { borrowMonth = 12; borrowYear--; }
    days += daysInMonth(borrowMonth, borrowYear);
    months--;
  }
  if (months < 0) { months += 12; years--; }

  out.years = years; out.months = months; out.days = days;
  out.hours = hours; out.minutes = minutes; out.seconds = seconds;
  return out;
}

// ---------------------------------------------------------------------
// Persistent config (NVS)

// ---------------------------------------------------------------------
// Web/filename safety helpers
// ---------------------------------------------------------------------
bool isValidBackgroundFilename(const String &filename) {
  if (filename.length() == 0 || filename.length() > 96) return false;
  if (filename == "." || filename == "..") return false;
  if (filename.indexOf('/') >= 0 || filename.indexOf('\\') >= 0) return false;
  if (filename.indexOf("..") >= 0) return false;

  for (size_t i = 0; i < filename.length(); ++i) {
    const char c = filename[i];
    const bool allowed =
        (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == ' ' || c == '_' || c == '-' || c == '.' ||
        c == '(' || c == ')';
    if (!allowed) return false;
  }

  String lower = filename;
  lower.toLowerCase();
  return lower.endsWith(".jpg") || lower.endsWith(".jpeg");
}

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);

  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char escaped[7];
          snprintf(escaped, sizeof(escaped), "\\u%04X", c);
          out += escaped;
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

// Return a valid JSON number even when a sensor value is NaN or infinite.
String jsonFloat(float value, unsigned int decimals, float fallback = -1.0f) {
  const float finiteValue = isfinite(value) ? value : fallback;
  return String(finiteValue, decimals);
}

// ---------------------------------------------------------------------
String sanitizeEventText(const String &input) {
  String out;
  out.reserve(47);
  for (size_t i = 0; i < input.length() && out.length() < 47; i++) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    if (c >= 0x20 && c != 0x7F) out += static_cast<char>(c);
  }
  out.trim();
  if (out.length() == 0) out = "TO GO..!!..";
  return out;
}

bool normalizeHostname(const String &input, String &normalized, String *error = nullptr) {
  normalized = input;
  normalized.trim();
  normalized.toLowerCase();

  if (normalized.length() < 1 || normalized.length() > HOSTNAME_MAX_LENGTH) {
    if (error) *error = "Hostname must be 1 to 32 characters";
    return false;
  }
  if (normalized[0] == '-' || normalized[normalized.length() - 1] == '-') {
    if (error) *error = "Hostname cannot begin or end with a hyphen";
    return false;
  }
  for (size_t i = 0; i < normalized.length(); ++i) {
    const char c = normalized[i];
    const bool valid = (c >= 'a' && c <= 'z') ||
                       (c >= '0' && c <= '9') || c == '-';
    if (!valid) {
      if (error) *error = "Hostname may contain only letters, numbers, and hyphens";
      return false;
    }
  }
  return true;
}

IPAddress parseIpOrDefault(const String &value, const IPAddress &fallback) {
  IPAddress parsed;
  return parsed.fromString(value) ? parsed : fallback;
}

void loadConfig() {
  prefs.begin("deskclock", true);
  target.year   = prefs.getInt("ty", 2027);
  target.month  = prefs.getInt("tmo", 1);
  target.day    = prefs.getInt("td", 1);
  target.hour   = prefs.getInt("th", 0);
  target.minute = prefs.getInt("tmi", 0);
  target.second = prefs.getInt("ts", 0);
  target = sanitizeTargetDate(target);
  String tz = prefs.getString("tz", DEFAULT_TZ);
  tz.toCharArray(tzString, sizeof(tzString));
  String ntp1 = prefs.getString("ntp1", DEFAULT_NTP_SERVER_1);
  String ntp2 = prefs.getString("ntp2", DEFAULT_NTP_SERVER_2);
  ntp1.trim();
  ntp2.trim();
  if (ntp1.length() == 0) ntp1 = DEFAULT_NTP_SERVER_1;
  if (ntp2.length() == 0) ntp2 = DEFAULT_NTP_SERVER_2;
  ntp1.toCharArray(ntpServer1, sizeof(ntpServer1));
  ntp2.toCharArray(ntpServer2, sizeof(ntpServer2));
  String savedHostname = prefs.getString("hostname", DEFAULT_HOSTNAME);
  String validHostname;
  if (!normalizeHostname(savedHostname, validHostname)) validHostname = DEFAULT_HOSTNAME;
  validHostname.toCharArray(networkHostname, sizeof(networkHostname));
  String eventText = sanitizeEventText(
      prefs.isKey("eventTxt") ? prefs.getString("eventTxt") : String("TO GO..!!.."));
  eventText.toCharArray(countdownEventText, sizeof(countdownEventText));
  networkDhcp = prefs.getBool("netDhcp", true);
  networkIp = parseIpOrDefault(
      prefs.isKey("netIp") ? prefs.getString("netIp") : String("192.168.1.50"),
      IPAddress(192,168,1,50));
  networkGateway = parseIpOrDefault(
      prefs.isKey("netGw") ? prefs.getString("netGw") : String("192.168.1.1"),
      IPAddress(192,168,1,1));
  networkSubnet = parseIpOrDefault(
      prefs.isKey("netMask") ? prefs.getString("netMask") : String("255.255.255.0"),
      IPAddress(255,255,255,0));
  networkDns1 = parseIpOrDefault(
      prefs.isKey("netDns1") ? prefs.getString("netDns1") : String("1.1.1.1"),
      IPAddress(1,1,1,1));
  networkDns2 = parseIpOrDefault(
      prefs.isKey("netDns2") ? prefs.getString("netDns2") : String("8.8.8.8"),
      IPAddress(8,8,8,8));
  dimPercent     = prefs.getInt("dimPct", DEFAULT_DIM_PERCENT);
  idleTimeoutSec = prefs.getInt("idleSec", DEFAULT_IDLE_TIMEOUT);
  dimBelowLux    = prefs.isKey("dimLux")
                       ? prefs.getFloat("dimLux")
                       : DEFAULT_DIM_BELOW_LUX;
  undimAboveLux  = prefs.isKey("undimLux")
                       ? prefs.getFloat("undimLux")
                       : DEFAULT_UNDIM_ABOVE_LUX;
  buzzerEnabled  = prefs.getBool("buzzEn", true);
  speakerMuted   = prefs.getBool("spkMute", false);
  buzzerTripC    = prefs.isKey("buzzTripC")
                       ? prefs.getFloat("buzzTripC")
                       : DEFAULT_BUZZER_TRIP_C;
  buzzerClearC   = prefs.isKey("buzzClearC")
                       ? prefs.getFloat("buzzClearC")
                       : DEFAULT_BUZZER_CLEAR_C;
  use12HourTime  = prefs.getBool("use12h", false);
  currentBgFile  = prefs.getString("bgFile", "");
  if (currentBgFile.length() > 0 && !isValidBackgroundFilename(currentBgFile)) {
    Serial.println("[config] Ignoring invalid saved background filename");
    currentBgFile = "";
  }
  bgRotationEnabled     = prefs.getBool("bgRotEn", false);
  bgRotationIntervalMin = prefs.getInt("bgRotMin", DEFAULT_BG_ROTATION_MIN);
  bgRotationShuffle     = prefs.getBool("bgRotShuf", false);
  prefs.end();
}

void saveNtpConfig(const String &primary, const String &secondary) {
  String ntp1 = primary;
  String ntp2 = secondary;
  ntp1.trim();
  ntp2.trim();
  if (ntp1.length() == 0) ntp1 = DEFAULT_NTP_SERVER_1;
  if (ntp2.length() == 0) ntp2 = DEFAULT_NTP_SERVER_2;
  ntp1 = ntp1.substring(0, sizeof(ntpServer1) - 1);
  ntp2 = ntp2.substring(0, sizeof(ntpServer2) - 1);

  prefs.begin("deskclock", false);
  prefs.putString("ntp1", ntp1);
  prefs.putString("ntp2", ntp2);
  prefs.end();

  ntp1.toCharArray(ntpServer1, sizeof(ntpServer1));
  ntp2.toCharArray(ntpServer2, sizeof(ntpServer2));
}

void saveEventText(const String &text) {
  String safe = sanitizeEventText(text);
  prefs.begin("deskclock", false);
  prefs.putString("eventTxt", safe);
  prefs.end();
  safe.toCharArray(countdownEventText, sizeof(countdownEventText));
}

void saveNetworkConfig(bool dhcp, const IPAddress &ip, const IPAddress &gateway,
                       const IPAddress &subnet, const IPAddress &dns1, const IPAddress &dns2) {
  prefs.begin("deskclock", false);
  prefs.putBool("netDhcp", dhcp);
  prefs.putString("netIp", ip.toString());
  prefs.putString("netGw", gateway.toString());
  prefs.putString("netMask", subnet.toString());
  prefs.putString("netDns1", dns1.toString());
  prefs.putString("netDns2", dns2.toString());
  prefs.end();
  networkDhcp = dhcp;
  networkIp = ip; networkGateway = gateway; networkSubnet = subnet;
  networkDns1 = dns1; networkDns2 = dns2;
}

void saveHostname(const String &hostname) {
  String normalized;
  if (!normalizeHostname(hostname, normalized)) normalized = DEFAULT_HOSTNAME;
  prefs.begin("deskclock", false);
  prefs.putString("hostname", normalized);
  prefs.end();
  normalized.toCharArray(networkHostname, sizeof(networkHostname));
}

void saveTimeFormat(bool use12h) {
  prefs.begin("deskclock", false);
  prefs.putBool("use12h", use12h);
  prefs.end();
}

void saveBuzzerConfig(bool enabled, bool muted, float tripC, float clearC) {
  prefs.begin("deskclock", false);
  prefs.putBool("buzzEn", enabled);
  prefs.putBool("spkMute", muted);
  prefs.putFloat("buzzTripC", tripC);
  prefs.putFloat("buzzClearC", clearC);
  prefs.end();
}

void saveLightThresholds(float dimLux, float undimLux) {
  prefs.begin("deskclock", false);
  prefs.putFloat("dimLux", dimLux);
  prefs.putFloat("undimLux", undimLux);
  prefs.end();
}

void saveBgRotationConfig(bool enabled, int intervalMin, bool shuffle) {
  prefs.begin("deskclock", false);
  prefs.putBool("bgRotEn", enabled);
  prefs.putInt("bgRotMin", max(intervalMin, MIN_BG_ROTATION_MIN));
  prefs.putBool("bgRotShuf", shuffle);
  prefs.end();
}

void saveAllConfig(TargetDate t, const String &tz, int dimPct, int idleSec) {
  t = sanitizeTargetDate(t);
  prefs.begin("deskclock", false);
  prefs.putInt("ty", t.year);
  prefs.putInt("tmo", t.month);
  prefs.putInt("td", t.day);
  prefs.putInt("th", t.hour);
  prefs.putInt("tmi", t.minute);
  prefs.putInt("ts", t.second);
  prefs.putString("tz", tz);
  prefs.putInt("dimPct", constrain(dimPct, 0, 100));
  prefs.putInt("idleSec", max(idleSec, 1));
  prefs.putBool("hasCfg", true);
  prefs.end();
}

void saveBgFile(const String &filename) {
  String safeFilename = filename;
  if (safeFilename.length() > 0 && !isValidBackgroundFilename(safeFilename)) safeFilename = "";
  prefs.begin("deskclock", false);
  prefs.putString("bgFile", safeFilename);
  prefs.end();
}

bool hasBeenConfigured() {
  prefs.begin("deskclock", true);
  bool has = prefs.getBool("hasCfg", false);
  prefs.end();
  return has;
}

// One-shot request consumed early during the next boot. This lets the web UI
// and the post-boot GPIO0 handler enter WiFiManager Setup Mode without erasing
// saved Wi-Fi credentials or requiring GPIO0 to remain held through reset.
void requestSetupModeOnNextBoot() {
  prefs.begin("deskclock", false);
  prefs.putBool("forceSetup", true);
  prefs.end();
}

bool consumeSetupModeRequest() {
  prefs.begin("deskclock", false);
  const bool requested = prefs.getBool("forceSetup", false);
  if (requested) prefs.remove("forceSetup");
  prefs.end();
  return requested;
}

// Capacitive touch uses controller coordinates and requires no user calibration.

// ---------------------------------------------------------------------
// Backlight / AXP2101 power sensing
// ---------------------------------------------------------------------
void backlightInit() {
  // Arduino-ESP32 2.x / PlatformIO espressif32 6.12 channel-based LEDC API.
  ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_FREQ, BACKLIGHT_PWM_RES);
  ledcAttachPin(BACKLIGHT_PIN, BACKLIGHT_PWM_CHANNEL);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, 255); // start at full brightness
}

void setBacklight(uint8_t level) {
  ledcWrite(BACKLIGHT_PWM_CHANNEL, level);
}

// Read USB/VBUS state directly from the onboard AXP2101.
void updateUsbPresence(bool forceRead = false) {
  unsigned long nowMs = millis();
  if (!forceRead && usbPresenceInitialized &&
      nowMs - lastUsbReadMs < USB_READ_INTERVAL_MS) return;
  lastUsbReadMs = nowMs;

  if (!pmuFound) {
    usbPresent = true; // conservative: avoid battery-only dimming until PMU is online
    inputRailVoltage = 0.0f;
    usbPresenceInitialized = true;
    return;
  }

  usbPresent = power.isVbusIn() && power.isVbusGood();
  inputRailVoltage = power.getVbusVoltage() / 1000.0f;
  usbPresenceInitialized = true;
}


void logEvent(const char *event, const String &details) {
  Serial.printf("[event] %s %s\n", event, details.c_str());
  if (!sdReady) return;

  if (SD.exists(I2C_EVENT_LOG_PATH)) {
    File existingLog = SD.open(I2C_EVENT_LOG_PATH, FILE_READ);
    size_t logSize = existingLog ? existingLog.size() : 0;
    if (existingLog) existingLog.close();
    if (logSize >= I2C_EVENT_LOG_MAX) {
      SD.remove("/logs/events.1.log");
      SD.rename(I2C_EVENT_LOG_PATH, "/logs/events.1.log");
    }
  }

  File logFile = SD.open(I2C_EVENT_LOG_PATH, FILE_APPEND);
  if (!logFile) {
    Serial.println("[log] unable to open /logs/events.log");
    return;
  }

  struct tm now;
  char timestamp[32];
  if (getLocalTime(&now, 10)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &now);
  } else {
    snprintf(timestamp, sizeof(timestamp), "uptime=%lu", (unsigned long)millis());
  }
  logFile.printf("%s,%s,%s\n", timestamp, event, details.c_str());
  logFile.close();
}


void rotateBatteryLogIfNeeded() {
  if (!sdReady || !SD.exists(BATTERY_LOG_PATH)) return;

  File existingLog = SD.open(BATTERY_LOG_PATH, FILE_READ);
  size_t logSize = existingLog ? existingLog.size() : 0;
  if (existingLog) existingLog.close();

  if (logSize >= BATTERY_LOG_MAX) {
    SD.remove(BATTERY_LOG_OLD_PATH);
    SD.rename(BATTERY_LOG_PATH, BATTERY_LOG_OLD_PATH);
  }
}

void logBatteryTelemetry(bool force = false) {
  if (!sdReady || !batteryFound || batteryVoltage < 0.0f || batteryPercent < 0.0f) return;

  const unsigned long nowMs = millis();
  if (!force && batteryLogHasSample && nowMs - lastBatteryLogMs < BATTERY_LOG_INTERVAL_MS) return;

  rotateBatteryLogIfNeeded();
  const bool needsHeader = !SD.exists(BATTERY_LOG_PATH);
  File logFile = SD.open(BATTERY_LOG_PATH, FILE_APPEND);
  if (!logFile) {
    Serial.println("[log] unable to open /logs/battery.csv");
    return;
  }

  if (needsHeader || logFile.size() == 0) {
    logFile.println("timestamp,uptime_s,power,battery_percent,battery_voltage_v,system_temp_c,system_temp_valid,input_voltage_v");
  }

  struct tm now;
  char timestamp[32];
  if (getLocalTime(&now, 10)) {
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &now);
  } else {
    snprintf(timestamp, sizeof(timestamp), "uptime=%lu", nowMs / 1000UL);
  }

  logFile.printf("%s,%lu,%s,%.1f,%.3f,",
                 timestamp,
                 nowMs / 1000UL,
                 usbPresent ? "USB" : "BATTERY",
                 batteryPercent,
                 batteryVoltage);
  if (systemTempValid && isfinite(systemTempC)) {
    logFile.printf("%.2f", systemTempC);
  }
  logFile.printf(",%d,%.3f\n", systemTempValid ? 1 : 0, inputRailVoltage);
  logFile.close();

  lastBatteryLogMs = nowMs;
  batteryLogHasSample = true;
}

bool prepareI2cBus() {
  // Prepare the custom ESP32-S3 pins without opening the bus here.
  // XPowersLib opens/reconfigures Wire while initializing the AXP2101; the
  // VEML7700 is intentionally initialized afterward on that final bus state.
  Wire.end();
  delay(10);
  i2cBusReady = false;
  if (!Wire.setPins(I2C_SDA_PIN, I2C_SCL_PIN)) {
    Serial.println("[i2c] Wire.setPins failed");
    return false;
  }
  Serial.printf("[i2c] prepared SDA=%d SCL=%d; AXP2101 will start bus\n",
                I2C_SDA_PIN, I2C_SCL_PIN);
  return true;
}

void finalizeI2cBus() {
  // Both PMU and light-sensor drivers now reference the same running Wire
  // instance. Apply the project-wide operating parameters once.
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
  i2cBusReady = true;
  delay(10);
  Serial.printf("[i2c] initialized SDA=%d SCL=%d clock=%lu timeout=%lums\n",
                I2C_SDA_PIN, I2C_SCL_PIN,
                (unsigned long)I2C_CLOCK_HZ,
                (unsigned long)I2C_TIMEOUT_MS);
}

bool probeI2cAddress(uint8_t address) {
  if (!i2cBusReady) return false;
  Wire.beginTransmission(address);
  return Wire.endTransmission(true) == 0;
}

void noteI2cSuccess(uint8_t &failureCounter) {
  failureCounter = 0;
}

void noteI2cFailure(const char *device, uint8_t address, uint8_t &failureCounter) {
  if (failureCounter < 255) failureCounter++;
  String details = "device=" + String(device) +
                   ",address=0x" + String(address, HEX) +
                   ",count=" + String(failureCounter);
  logEvent("I2C_ERROR", details);
  if (failureCounter >= I2C_FAILURES_BEFORE_RECOVERY) {
    i2cRecoveryRequested = true;
  }
}

void lightSensorInit() {
  lightSensorFound = false;

  // XPowersLib has already opened Wire for the AXP2101. Starting the Adafruit
  // device now may emit a harmless "Bus already started" warning, but it keeps
  // the VEML7700 bound to the final shared-bus configuration. finalizeI2cBus()
  // then reapplies the common 100 kHz clock and timeout.
  lightSensorFound = veml.begin(&Wire);
  finalizeI2cBus();
  if (!lightSensorFound) {
    noteI2cFailure("VEML7700", VEML7700_I2C_ADDRESS, lightI2cFailures);
    return;
  }

  float lux = veml.readLux();
  if (isfinite(lux) && lux >= 0.0f) {
    currentLux = lux;
    noteI2cSuccess(lightI2cFailures);
  } else {
    lightSensorFound = false;
    noteI2cFailure("VEML7700", VEML7700_I2C_ADDRESS, lightI2cFailures);
  }
}

// Same hysteresis pattern as updateUsbPresence(): once dark, stays dark
// until lux rises above undimAboveLux; once bright, stays bright until it
// falls below dimBelowLux. Avoids flicker right at a single threshold.
void updateLightSensor() {
  if (!lightSensorFound) return;
  if (millis() - lastLuxReadMs < LUX_READ_INTERVAL_MS) return;
  lastLuxReadMs = millis();

  if (!probeI2cAddress(VEML7700_I2C_ADDRESS)) {
    noteI2cFailure("VEML7700", VEML7700_I2C_ADDRESS, lightI2cFailures);
    return;
  }

  float newLux = veml.readLux();
  if (!isfinite(newLux) || newLux < 0.0f) {
    noteI2cFailure("VEML7700", VEML7700_I2C_ADDRESS, lightI2cFailures);
    return;
  }

  currentLux = newLux;
  noteI2cSuccess(lightI2cFailures);
  if (!ambientDark && currentLux < dimBelowLux) {
    ambientDark = true;
  } else if (ambientDark && currentLux > undimAboveLux) {
    ambientDark = false;
  }
}

void updateBacklight() {
  updateUsbPresence();
  updateLightSensor();

  bool recentlyActive = (millis() - lastActivityMs) < (unsigned long)idleTimeoutSec * 1000UL;

  // Two independent reasons to dim -- battery conservation (existing) and
  // low ambient light (new) -- either one can trigger it. Both are gated by
  // recentlyActive so a touch/button press wakes the screen to full
  // brightness regardless of which reason caused the dim, and regardless of
  // whether the room is still dark or still on battery.
  bool wantDimForBattery = !usbPresent && !recentlyActive;
  bool wantDimForLight = lightSensorFound && ambientDark && !recentlyActive;
  bool shouldBeDim = wantDimForBattery || wantDimForLight;

  if (shouldBeDim != backlightDimmed) {
    backlightDimmed = shouldBeDim;
    uint8_t level = backlightDimmed ? map(dimPercent, 0, 100, 0, 255) : 255;
    setBacklight(level);
  }
}

void registerActivity() {
  lastActivityMs = millis();
}

// ---------------------------------------------------------------------
// Onboard AXP2101 battery and power management
// ---------------------------------------------------------------------
void updateSystemTemperature(bool forceRead = false) {
  if (!pmuFound) {
    systemTempValid = false;
    systemTempFault = true;
    systemTempC = NAN;
    return;
  }

  const uint16_t raw = power.getTsPinValue();
  const float sampleC = power.getTsTemperature();
  const bool rawFault = (raw == 0U || raw == 0x2000U || raw == 0xFFFFU);
  const bool plausible = !rawFault && isfinite(sampleC) && sampleC >= -20.0f && sampleC <= 85.0f;

  if (plausible) {
    if (systemTempValidSamples < 255U) systemTempValidSamples++;
    systemTempFaultSamples = 0;

    // A light IIR filter suppresses ADC noise without hiding real enclosure changes.
    systemTempC = isfinite(systemTempC) ? (systemTempC * 0.75f + sampleC * 0.25f) : sampleC;
    if (forceRead || systemTempValidSamples >= 2U) {
      systemTempValid = true;
      systemTempFault = false;
    }
  } else {
    systemTempValidSamples = 0;
    if (systemTempFaultSamples < 255U) systemTempFaultSamples++;
    if (systemTempFaultSamples >= 3U) {
      systemTempValid = false;
      systemTempFault = true;
      systemTempC = NAN;
    }
  }
}

void batteryInit() {
  pmuFound = power.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA_PIN, I2C_SCL_PIN);
  systemTempValid = false;
  systemTempFault = false;
  systemTempValidSamples = 0;
  systemTempFaultSamples = 0;
  systemTempC = NAN;

  if (!pmuFound) {
    batteryFound = false;
    systemTempFault = true;
    noteI2cFailure("AXP2101", AXP2101_I2C_ADDRESS, batteryI2cFailures);
    return;
  }

  // R29 is the board-mounted 10 kOhm NTC connected to the AXP2101 TS input.
  // enableTSPinMeasure() enables the TS excitation/ADC path used by
  // getTsPinValue() and getTsTemperature().
  power.enableTSPinMeasure();
  power.enableBattDetection();
  power.enableVbusVoltageMeasure();
  power.enableBattVoltageMeasure();
  power.enableSystemVoltageMeasure();
  updateUsbPresence(true);

  batteryFound = power.isBatteryConnect();
  if (batteryFound) {
    batteryVoltage = power.getBattVoltage() / 1000.0f;
    batteryPercent = power.getBatteryPercent();
  } else {
    batteryVoltage = -1.0f;
    batteryPercent = -1.0f;
  }

  delay(20); // allow the newly enabled TS ADC path to settle
  updateSystemTemperature(true);
  noteI2cSuccess(batteryI2cFailures);
  lastBatteryReadMs = millis();
}

void updateBattery() {
  if (!pmuFound) return;
  if (millis() - lastBatteryReadMs < BATTERY_READ_MS) return;
  lastBatteryReadMs = millis();

  if (!probeI2cAddress(AXP2101_I2C_ADDRESS)) {
    noteI2cFailure("AXP2101", AXP2101_I2C_ADDRESS, batteryI2cFailures);
    return;
  }

  updateUsbPresence(true);
  batteryFound = power.isBatteryConnect();
  if (batteryFound) {
    batteryVoltage = power.getBattVoltage() / 1000.0f;
    batteryPercent = power.getBatteryPercent();
  } else {
    batteryVoltage = -1.0f;
    batteryPercent = -1.0f;
  }
  updateSystemTemperature();
  noteI2cSuccess(batteryI2cFailures);
}

unsigned long currentI2cRecoveryIntervalMs() {
  if (i2cRecoveryFailureStreak < I2C_RECOVERY_FAST_FAILURES) {
    return I2C_RECOVERY_FAST_MS;
  }
  if (i2cRecoveryFailureStreak < I2C_RECOVERY_MEDIUM_FAILURES) {
    return I2C_RECOVERY_MEDIUM_MS;
  }
  return I2C_RECOVERY_SLOW_MS;
}

bool shouldLogI2cRecoveryAttempt() {
  // Log the first few attempts, each backoff-tier transition, and occasional
  // long-running failures. This preserves useful diagnostics without turning
  // a disconnected sensor into continuous SD-card writes.
  if (i2cRecoveryFailureStreak < I2C_RECOVERY_FAST_FAILURES) return true;
  if (i2cRecoveryFailureStreak == I2C_RECOVERY_FAST_FAILURES ||
      i2cRecoveryFailureStreak == I2C_RECOVERY_MEDIUM_FAILURES) return true;
  if (i2cRecoveryFailureStreak < I2C_RECOVERY_MEDIUM_FAILURES) {
    return (i2cRecoveryFailureStreak % 5U) == 0U;
  }
  return (i2cRecoveryFailureStreak % 12U) == 0U;
}

void serviceI2cRecovery() {
  if (!i2cRecoveryRequested) return;

  const unsigned long recoveryIntervalMs = currentI2cRecoveryIntervalMs();
  if (millis() - lastI2cRecoveryMs < recoveryIntervalMs) return;

  lastI2cRecoveryMs = millis();
  i2cRecoveryRequested = false;
  i2cRecoveryCount++;
  const bool logThisAttempt = shouldLogI2cRecoveryAttempt();

  buzzerAlarming = false;
  systemTempValid = false;
  pmuFound = false;
  batteryFound = false;
  lightSensorFound = false;

  if (logThisAttempt) {
    logEvent("I2C_RECOVERY_START",
             "attempt=" + String(i2cRecoveryCount) +
             ",streak=" + String(i2cRecoveryFailureStreak) +
             ",intervalMs=" + String(recoveryIntervalMs));
  }

  batteryI2cFailures = 0;
  lightI2cFailures = 0;
  bool busPrepared = prepareI2cBus();
  if (busPrepared) {
    // XPowersLib starts/reconfigures Wire when the AXP2101 is opened. Do that
    // first, then bind the VEML7700 to the final shared-bus state.
    batteryInit();
    lightSensorInit();
  }
  bool busOk = busPrepared && i2cBusReady;

  bool batteryRecovered = !batteryI2cExpected || pmuFound;
  bool lightRecovered = !lightI2cExpected || lightSensorFound;
  bool recovered = busOk && batteryRecovered && lightRecovered;
  String details = "attempt=" + String(i2cRecoveryCount) +
                   ",streak=" + String(i2cRecoveryFailureStreak) +
                   ",bus=" + String(busOk ? 1 : 0) +
                   ",pmu=" + String(pmuFound ? 1 : 0) +
           ",battery=" + String(batteryFound ? 1 : 0) +
                   ",light=" + String(lightSensorFound ? 1 : 0);

  if (recovered) {
    // Always log the transition back to healthy operation, then restore the
    // fast tier for any future, unrelated fault.
    logEvent("I2C_RECOVERY_OK", details);
    i2cRecoveryFailureStreak = 0;
  } else {
    if (logThisAttempt) {
      logEvent("I2C_RECOVERY_PARTIAL", details);
    }
    if (i2cRecoveryFailureStreak < 65535U) i2cRecoveryFailureStreak++;
    i2cRecoveryRequested = true;
  }
}

bool es8311Write(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(ES8311_I2C_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool es8311Read(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(ES8311_I2C_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)ES8311_I2C_ADDRESS, 1, true) != 1) return false;
  value = Wire.read();
  return true;
}

bool es8311SetMute(bool mute) {
  if (!speakerCodecReady) return false;
  uint8_t reg31 = 0;
  if (!es8311Read(0x31, reg31)) return false;
  if (mute) reg31 |= 0x60;      // mute DAC left/right
  else      reg31 &= ~0x60;
  return es8311Write(0x31, reg31);
}

bool initEs8311For16kHz() {
  // 16 kHz, 16-bit I2S, MCLK=4.096 MHz (256 fs). Register values follow
  // the ES8311 clock coefficient table used by the WaveShare reference driver.
  bool ok = true;
  ok &= es8311Write(0x00, 0x1F);
  delay(20);
  ok &= es8311Write(0x00, 0x00);
  ok &= es8311Write(0x00, 0x80);
  ok &= es8311Write(0x01, 0x3F);
  ok &= es8311Write(0x02, 0x00);
  ok &= es8311Write(0x03, 0x10);
  ok &= es8311Write(0x04, 0x10);
  ok &= es8311Write(0x05, 0x00);
  ok &= es8311Write(0x06, 0x03);
  ok &= es8311Write(0x07, 0x00);
  ok &= es8311Write(0x08, 0xFF);
  ok &= es8311Write(0x09, 0x0C);
  ok &= es8311Write(0x0A, 0x0C);
  ok &= es8311Write(0x0D, 0x01);
  ok &= es8311Write(0x0E, 0x02);
  ok &= es8311Write(0x12, 0x00);
  ok &= es8311Write(0x13, 0x10);
  ok &= es8311Write(0x1C, 0x6A);
  ok &= es8311Write(0x37, 0x08);
  ok &= es8311Write(0x32, 0xB2); // approximately 70 percent DAC volume
  ok &= es8311Write(0x31, 0x60); // start muted
  return ok;
}

bool initSpeakerI2s() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SPEAKER_SAMPLE_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = SPEAKER_FIXED_MCLK;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = SPEAKER_I2S_MCLK;
  pins.bck_io_num = SPEAKER_I2S_BCLK;
  pins.ws_io_num = SPEAKER_I2S_LRCK;
  pins.data_out_num = SPEAKER_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;

  esp_err_t err = i2s_driver_install(SPEAKER_I2S_PORT, &cfg, 0, nullptr);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return false;
  if (i2s_set_pin(SPEAKER_I2S_PORT, &pins) != ESP_OK) return false;
  if (i2s_set_clk(SPEAKER_I2S_PORT, SPEAKER_SAMPLE_RATE,
                  I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO) != ESP_OK) return false;
  i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
  return true;
}

uint32_t readLe32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint16_t readLe16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

bool openSpeakerWav(const char *path) {
  if (!sdReady || !path || !SD.exists(path)) return false;
  speakerFile = SD.open(path, FILE_READ);
  if (!speakerFile) return false;

  uint8_t header[44];
  if (speakerFile.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "RIFF", 4) != 0 ||
      memcmp(header + 8, "WAVE", 4) != 0 ||
      memcmp(header + 12, "fmt ", 4) != 0 ||
      readLe16(header + 20) != 1 ||
      readLe16(header + 22) != 1 ||
      readLe32(header + 24) != SPEAKER_SAMPLE_RATE ||
      readLe16(header + 34) != 16 ||
      memcmp(header + 36, "data", 4) != 0) {
    speakerFile.close();
    return false;
  }

  speakerDataRemaining = readLe32(header + 40);
  return speakerDataRemaining > 0;
}

void stopSpeakerPlayback() {
  if (speakerFile) speakerFile.close();
  speakerDataRemaining = 0;
  speakerPlaying = false;
  speakerPlaybackKind = SpeakerPlaybackKind::None;
  if (speakerCodecReady) es8311SetMute(true);
  if (speakerCodecReady) i2s_zero_dma_buffer(SPEAKER_I2S_PORT);
}

void speakerInit() {
  speakerCodecReady = false;
  speakerPlaying = false;
  speakerFileAvailable = sdReady && SD.exists(SPEAKER_ALERT_WAV_PATH);
  celebrationFileAvailable = sdReady && SD.exists(COUNTDOWN_CELEBRATION_WAV_PATH);

  if (!i2cBusReady) {
    Serial.println("[speaker] skipped: I2C unavailable");
    return;
  }

  if (!initSpeakerI2s()) {
    Serial.println("[speaker] I2S initialization failed");
    return;
  }

  speakerCodecReady = initEs8311For16kHz();
  if (!speakerCodecReady) {
    Serial.println("[speaker] ES8311 initialization failed");
    return;
  }

  // PA_CTRL is TCA9554 EXIO7. The helper is defined in the setup section.
  extern bool setSpeakerAmplifier(bool enabled);
  setSpeakerAmplifier(true);
  es8311SetMute(true);

  Serial.printf("[speaker] codec=1 tempWav=%d celebrationWav=%d\n",
                speakerFileAvailable ? 1 : 0,
                celebrationFileAvailable ? 1 : 0);
  Serial.printf("[speaker] temp=%s celebration=%s\n",
                SPEAKER_ALERT_WAV_PATH, COUNTDOWN_CELEBRATION_WAV_PATH);
}

void updateBuzzer() {
  if (!buzzerEnabled || !pmuFound || !systemTempValid) {
    if (buzzerAlarming) logEvent("SYSTEM_TEMP_CLEAR", "reason=inhibited");
    buzzerAlarming = false;
    return;
  }

  if (!buzzerAlarming && systemTempC >= buzzerTripC) {
    buzzerAlarming = true;
    lastSpeakerSequenceMs = 0;
    logEvent("SYSTEM_TEMP_ALARM",
             "tempC=" + String(systemTempC, 1) +
             ",tripC=" + String(buzzerTripC, 1));
  } else if (buzzerAlarming && systemTempC <= buzzerClearC) {
    buzzerAlarming = false;
    logEvent("SYSTEM_TEMP_CLEAR",
             "tempC=" + String(systemTempC, 1) +
             ",clearC=" + String(buzzerClearC, 1));
  }
}

bool startSpeakerWav(const char *path, SpeakerPlaybackKind kind) {
  if (!openSpeakerWav(path)) return false;
  es8311SetMute(false);
  speakerPlaying = true;
  speakerPlaybackKind = kind;
  return true;
}

void updateCountdownCelebration(bool reached) {
  // Establish the initial state without sounding after a reboot when the
  // configured target was already in the past.
  if (!countdownReachedStateKnown) {
    countdownReachedStateKnown = true;
    countdownWasReached = reached;
    return;
  }

  if (reached && !countdownWasReached) {
    const bool permitted = buzzerEnabled && !speakerMuted && speakerCodecReady &&
                           celebrationFileAvailable && sdReady;
    if (permitted) {
      celebrationPending = true;
      logEvent("COUNTDOWN_REACHED", "celebration=queued");
    } else {
      logEvent("COUNTDOWN_REACHED", "celebration=suppressed");
    }
  }
  countdownWasReached = reached;
}

void serviceSpeakerAudio() {
  const bool audioEnabled = buzzerEnabled && !speakerMuted &&
                            speakerCodecReady && sdReady;

  if (!audioEnabled) {
    celebrationPending = false;
    if (speakerPlaying) stopSpeakerPlayback();
    return;
  }

  // A one-time countdown celebration takes priority over the repeating
  // temperature alert. If necessary, interrupt the alert sequence and resume
  // normal thermal-alert service after the celebration has finished.
  if (celebrationPending) {
    if (speakerPlaying && speakerPlaybackKind != SpeakerPlaybackKind::CountdownCelebration) {
      stopSpeakerPlayback();
    }
    if (!speakerPlaying) {
      if (!startSpeakerWav(COUNTDOWN_CELEBRATION_WAV_PATH,
                           SpeakerPlaybackKind::CountdownCelebration)) {
        celebrationFileAvailable = false;
        celebrationPending = false;
        Serial.println("[speaker] celebration WAV open/format failure");
        return;
      }
      Serial.println("[speaker] countdown celebration started");
    }
  } else {
    const bool thermalPermitted = buzzerAlarming && speakerFileAvailable;
    if (!thermalPermitted) {
      if (speakerPlaying) stopSpeakerPlayback();
      return;
    }

    if (!speakerPlaying) {
      if (lastSpeakerSequenceMs != 0 &&
          millis() - lastSpeakerSequenceMs < SPEAKER_REPEAT_INTERVAL_MS) return;

      if (!startSpeakerWav(SPEAKER_ALERT_WAV_PATH,
                           SpeakerPlaybackKind::TemperatureAlert)) {
        speakerFileAvailable = false;
        Serial.println("[speaker] alert WAV open/format failure");
        return;
      }
      lastSpeakerSequenceMs = millis();
    }
  }

  int16_t mono[256];
  int16_t stereo[512];
  const size_t wanted = (speakerDataRemaining < sizeof(mono)) ? speakerDataRemaining : sizeof(mono);
  const size_t got = speakerFile.read((uint8_t *)mono, wanted);

  if (got == 0) {
    const bool celebrationFinished =
        speakerPlaybackKind == SpeakerPlaybackKind::CountdownCelebration;
    stopSpeakerPlayback();
    if (celebrationFinished) celebrationPending = false;
    return;
  }

  const size_t samples = got / sizeof(int16_t);
  for (size_t i = 0; i < samples; ++i) {
    stereo[i * 2] = mono[i];
    stereo[i * 2 + 1] = mono[i];
  }

  size_t written = 0;
  i2s_write(SPEAKER_I2S_PORT, stereo, samples * 2 * sizeof(int16_t),
            &written, pdMS_TO_TICKS(20));

  speakerDataRemaining -= got;
  if (speakerDataRemaining == 0) {
    const bool celebrationFinished =
        speakerPlaybackKind == SpeakerPlaybackKind::CountdownCelebration;
    stopSpeakerPlayback();
    if (celebrationFinished) {
      celebrationPending = false;
      Serial.println("[speaker] countdown celebration finished");
    }
  }
}

// ---------------------------------------------------------------------
// Background images (SD card + PSRAM buffer)
// ---------------------------------------------------------------------
bool ensureBgBuffer() {
  if (bgBuffer) return true;
  bgBuffer = (uint16_t *)ps_malloc((size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t));
  if (!bgBuffer) {
    Serial.println("Background buffer allocation failed (PSRAM full/unavailable)");
    return false;
  }
  return true;
}

// TJpg_Decoder callback: writes each decoded block into our full-screen
// PSRAM buffer instead of straight to the display.
bool jpegToBufferCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
  if (!bgBuffer) return false;
  for (int16_t row = 0; row < h; row++) {
    int16_t py = y + row;
    if (py < 0 || py >= SCREEN_H) continue;
    for (int16_t col = 0; col < w; col++) {
      int16_t px = x + col;
      if (px < 0 || px >= SCREEN_W) continue;
      bgBuffer[py * SCREEN_W + px] = bitmap[row * w + col];
    }
  }
  return true;
}

// Decodes filename (a JPEG on the SD card, under BACKGROUNDS_DIR) into
// bgBuffer, auto-scaling and centering it to fit the 480x320 screen.
bool loadBackgroundFromSD(const String &filename) {
  if (!sdReady || !isValidBackgroundFilename(filename)) return false;
  if (!ensureBgBuffer()) return false;

  String path = String(BACKGROUNDS_DIR) + "/" + filename;
  if (!SD.exists(path)) {
    Serial.printf("Background file not found: %s\n", path.c_str());
    return false;
  }

  uint16_t w = 0, h = 0;
  TJpgDec.setJpgScale(1);
  if (TJpgDec.getFsJpgSize(&w, &h, path.c_str(), SD) != JDR_OK || w == 0 || h == 0) {
    Serial.printf("Could not read JPEG size: %s\n", path.c_str());
    return false;
  }

  // Pick the largest scale (1/2/4/8) that still fits the screen
  uint8_t scale = 1;
  uint16_t sw = w, sh = h;
  while ((sw > SCREEN_W || sh > SCREEN_H) && scale < 8) {
    scale *= 2;
    sw = w / scale;
    sh = h / scale;
  }
  TJpgDec.setJpgScale(scale);

  int16_t offsetX = (SCREEN_W - (int16_t)sw) / 2;
  int16_t offsetY = (SCREEN_H - (int16_t)sh) / 2;
  if (offsetX < 0) offsetX = 0;
  if (offsetY < 0) offsetY = 0;

  memset(bgBuffer, 0, (size_t)SCREEN_W * SCREEN_H * sizeof(uint16_t)); // letterbox borders = black

  TJpgDec.setCallback(jpegToBufferCallback);
  JRESULT res = TJpgDec.drawFsJpg(offsetX, offsetY, path.c_str(), SD);
  if (res != JDR_OK) {
    Serial.printf("JPEG decode failed (%d): %s\n", res, path.c_str());
    return false;
  }
  return true;
}

void sdInit() {
  if (!SD_MMC.setPins(SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN)) {
    Serial.println("[sd] SD_MMC pin assignment failed");
    sdReady = false;
    return;
  }
  sdReady = SD_MMC.begin("/sdcard", true, false, 20000);
  if (!sdReady) {
    Serial.println("SD init failed -- backgrounds disabled, everything else still works.");
    return;
  }
  if (!SD.exists(BACKGROUNDS_DIR)) {
    SD.mkdir(BACKGROUNDS_DIR);
  }
  if (!SD.exists(LOGS_DIR)) {
    SD.mkdir(LOGS_DIR);
  }
}

// Returns a comma-free list of filenames in /backgrounds (for the web UI)
std::vector<String> listBackgroundFiles() {
  std::vector<String> files;
  if (!sdReady) return files;
  File dir = SD.open(BACKGROUNDS_DIR);
  if (!dir) return files;
  File entry = dir.openNextFile();
  while (entry) {
    if (!entry.isDirectory()) {
      String name = String(entry.name());
      int slash = name.lastIndexOf('/');
      if (slash >= 0) name = name.substring(slash + 1);
      if (isValidBackgroundFilename(name)) files.push_back(name);
    }
    entry = dir.openNextFile();
  }
  return files;
}

// Applies (or clears) the active background: loads into bgBuffer, saves the
// selection, and forces every screen to fully repaint next update.
// Forward declarations -- defined later in the file, needed here since
// applyBackground() must trigger a full repaint of whichever screen is
// currently showing (see comment below).
void drawCountdownStatic();
void drawDaysRemainingStatic();
void drawClockStatic();

void applyBackground(const String &filename) {
  if (filename.length() > 0 && !isValidBackgroundFilename(filename)) {
    Serial.println("Rejected invalid background filename");
    bgLoadFailed = true;
    return;
  }
  if (filename.length() == 0) {
    currentBgFile = "";
    bgLoadFailed = false;
  } else {
    bgLoadFailed = !loadBackgroundFromSD(filename);
    currentBgFile = bgLoadFailed ? "" : filename;
  }
  saveBgFile(currentBgFile);

  // Full repaint of the currently-displayed screen, not just the small
  // per-element text caches -- otherwise only the tiny text regions get
  // redrawn against the new image on the next tick, while everything else
  // on screen keeps showing the OLD background until the next manual
  // screen switch (which is what was showing up as stray black bars where
  // the old and new images' letterboxing didn't line up).
  if (currentScreen == SCREEN_COUNTDOWN) {
    drawCountdownStatic();
  } else if (currentScreen == SCREEN_DAYS_REMAINING) {
    drawDaysRemainingStatic();
  } else {
    drawClockStatic();
  }
}

bool hasBackground() {
  return bgBuffer != nullptr && currentBgFile.length() > 0;
}

// Checks whether it's time to auto-rotate, and if so, advances to the next
// background. Uses its own tracked position (bgRotationIndex) rather than
// whatever's currently displayed, so a manual pick via the web page is
// simply overridden at the next interval instead of derailing the sequence.
void rotateBackgroundIfDue() {
  if (!bgRotationEnabled || !sdReady) return;
  unsigned long intervalMs = (unsigned long)bgRotationIntervalMin * 60000UL;
  if (millis() - lastBgRotationMs < intervalMs) return;
  lastBgRotationMs = millis();

  std::vector<String> files = listBackgroundFiles();
  if (files.size() <= 1) return; // 0 or 1 image -- nothing meaningful to rotate to, true no-op

  // Try up to one full pass over the list; silently move to the next
  // candidate on a failed load rather than sitting on a black screen for a
  // whole interval because one file happened to be corrupt.
  int attemptsRemaining = (int)files.size();
  while (attemptsRemaining-- > 0) {
    if (bgRotationShuffle) {
      int newIndex;
      do {
        newIndex = random(files.size());
      } while (newIndex == bgRotationIndex);
      bgRotationIndex = newIndex;
    } else {
      bgRotationIndex = (bgRotationIndex + 1) % (int)files.size();
    }

    applyBackground(files[bgRotationIndex]);
    if (!bgLoadFailed) return; // success
    // else: silently try the next candidate
  }
  // Every image in the list failed to load -- give up quietly for this
  // cycle. The last applyBackground() call already left us in the safe
  // solid-black fallback state; next interval will try again from here.
}

// Restores a screen rectangle from the background buffer (or plain black if
// no background is active). Full-width rectangles are pushed as one
// contiguous block; anything narrower is restored row-by-row.
void restoreBackgroundRect(int x, int y, int w, int h) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > SCREEN_W) w = SCREEN_W - x;
  if (y + h > SCREEN_H) h = SCREEN_H - y;
  if (w <= 0 || h <= 0) return;

  if (!hasBackground()) {
    tft.fillRect(x, y, w, h, TFT_BLACK);
    return;
  }

  if (x == 0 && w == SCREEN_W) {
    tft.pushImage(x, y, w, h, &bgBuffer[(size_t)y * SCREEN_W]);
  } else {
    for (int row = 0; row < h; row++) {
      tft.pushImage(x, y + row, w, 1, &bgBuffer[(size_t)(y + row) * SCREEN_W + x]);
    }
  }
}

// Draws the full background (or fills black) across the whole screen --
// used when a screen is first (re)drawn.
void paintFullBackground() {
  if (hasBackground()) {
    tft.pushImage(0, 0, SCREEN_W, SCREEN_H, bgBuffer);
  } else {
    tft.fillScreen(TFT_BLACK);
  }
}

// Sets text color for drawing over the background: transparent (draws over
// whatever's already on screen) if a background image is active, otherwise
// opaque with a solid fill color (matches the original flat-black look).
void setOverlayTextColor(uint16_t fg) {
  if (hasBackground()) {
    tft.setTextColor(fg); // single-arg = transparent background
  } else {
    tft.setTextColor(fg, TFT_BLACK);
  }
}

// ---------------------------------------------------------------------
// Setup / config portal (first boot, or button held at cold boot)
// ---------------------------------------------------------------------
void showMessage(const String &line1, const String &line2 = "", const String &line3 = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(line1, tft.width() / 2, tft.height() / 2 - 15, 4);
  if (line2.length()) {
    tft.drawString(line2, tft.width() / 2, tft.height() / 2 + 25, 2);
  }
  if (line3.length()) {
    tft.drawString(line3, tft.width() / 2, tft.height() / 2 + 50, 2);
  }
}

void runConfigPortal(bool forced) {
  showMessage("Setup Mode", "Connect to WiFi: " AP_NAME, "Then browse to " + AP_IP.toString());

  loadConfig(); // pre-fill fields with current saved values

  snprintf(p_year, sizeof(p_year), "%d", target.year);
  snprintf(p_month, sizeof(p_month), "%d", target.month);
  snprintf(p_day, sizeof(p_day), "%d", target.day);
  snprintf(p_hour, sizeof(p_hour), "%d", target.hour);
  snprintf(p_minute, sizeof(p_minute), "%d", target.minute);
  snprintf(p_second, sizeof(p_second), "%d", target.second);
  strncpy(p_tz, tzString, sizeof(p_tz));
  snprintf(p_dim, sizeof(p_dim), "%d", dimPercent);
  snprintf(p_idle, sizeof(p_idle), "%d", idleTimeoutSec);

  WiFiManagerParameter custom_title("<p><b>Countdown Target (local time)</b></p>");
  WiFiManagerParameter p_ty("ty", "Target Year",   p_year,   6, "type='number' min='2024' max='2100'");
  WiFiManagerParameter p_tmo("tmo", "Target Month (1-12)", p_month, 4, "type='number' min='1' max='12'");
  WiFiManagerParameter p_td("td", "Target Day (1-31)",   p_day,   4, "type='number' min='1' max='31'");
  WiFiManagerParameter p_th("th", "Target Hour (0-23)",  p_hour,  4, "type='number' min='0' max='23'");
  WiFiManagerParameter p_tmi("tmi", "Target Minute (0-59)", p_minute, 4, "type='number' min='0' max='59'");
  WiFiManagerParameter p_ts("ts", "Target Second (0-59)", p_second, 4, "type='number' min='0' max='59'");
  WiFiManagerParameter tz_title("<p><b>Timezone</b> (POSIX TZ string, e.g. EST5EDT,M3.2.0,M11.1.0)</p>");
  WiFiManagerParameter p_tzp("tz", "Timezone String", p_tz, 64);
  WiFiManagerParameter dim_title("<p><b>Display / Battery</b></p>");
  WiFiManagerParameter p_dimp("dim", "Dim brightness % (on battery, idle)", p_dim, 4, "type='number' min='0' max='100'");
  WiFiManagerParameter p_idlep("idle", "Idle seconds before dimming", p_idle, 5, "type='number' min='1' max='3600'");

  wm.addParameter(&custom_title);
  wm.addParameter(&p_ty);
  wm.addParameter(&p_tmo);
  wm.addParameter(&p_td);
  wm.addParameter(&p_th);
  wm.addParameter(&p_tmi);
  wm.addParameter(&p_ts);
  wm.addParameter(&tz_title);
  wm.addParameter(&p_tzp);
  wm.addParameter(&dim_title);
  wm.addParameter(&p_dimp);
  wm.addParameter(&p_idlep);

  wm.setSaveConfigCallback(saveConfigCallback);
  wm.setConfigPortalTimeout(300); // reboot after 5 min idle
  wm.setAPStaticIPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(networkHostname);
  wm.setHostname(networkHostname);
  Serial.printf("[boot] hostname=%s.local\n", networkHostname);

  if (!networkDhcp) {
    WiFi.config(networkIp, networkGateway, networkSubnet, networkDns1, networkDns2);
    Serial.printf("[boot] WiFi static config ip=%s gateway=%s subnet=%s dns1=%s dns2=%s\n",
                  networkIp.toString().c_str(), networkGateway.toString().c_str(),
                  networkSubnet.toString().c_str(), networkDns1.toString().c_str(),
                  networkDns2.toString().c_str());
  } else {
    Serial.println("[boot] WiFi network mode=DHCP");
  }

  bool connected;
  if (forced) {
    connected = wm.startConfigPortal(AP_NAME, AP_PASS);
  } else {
    connected = wm.autoConnect(AP_NAME, AP_PASS);
  }

  if (shouldSaveConfig) {
    TargetDate t;
    t.year   = atoi(p_ty.getValue());
    t.month  = atoi(p_tmo.getValue());
    t.day    = atoi(p_td.getValue());
    t.hour   = atoi(p_th.getValue());
    t.minute = atoi(p_tmi.getValue());
    t.second = atoi(p_ts.getValue());
    t = sanitizeTargetDate(t);
    int dimPct  = atoi(p_dimp.getValue());
    int idleSec = atoi(p_idlep.getValue());
    saveAllConfig(t, String(p_tzp.getValue()), dimPct, idleSec);
    showMessage("Saved!", "Rebooting...");
    delay(1500);
    ESP.restart();
  }

  if (!connected) {
    showMessage("Setup timed out", "Rebooting...");
    delay(1500);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------
// Live web config page (available once connected to home WiFi)
// ---------------------------------------------------------------------
String htmlPage(const String &statusMsg = "") {
  struct tm now;
  char nowBuf[32] = "unknown";
  if (getLocalTime(&now, 200)) {
    strftime(nowBuf, sizeof(nowBuf), "%Y-%m-%d %H:%M:%S", &now);
  }

  String html = "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>Desk Clock Config</title>";
  html += "<style>body{font-family:sans-serif;max-width:420px;margin:30px auto;padding:0 16px;}";
  html += "label{display:block;margin-top:10px;font-weight:bold;}";
  html += "input,select{width:100%;padding:8px;box-sizing:border-box;font-size:16px;}";
  html += "button{margin-top:18px;padding:10px 20px;font-size:16px;}";
  html += "h3{margin-top:24px;border-top:1px solid #ccc;padding-top:14px;}";
  html += ".msg{background:#dff0d8;padding:10px;border-radius:4px;}";
  html += ".warn{background:#fcf8e3;padding:10px;border-radius:4px;}";
  html += ".status{color:#555;font-size:14px;}";
  html += ".bglist{margin-top:10px;}";
  html += ".bglist label{font-weight:normal;display:flex;align-items:center;gap:8px;margin-top:6px;}";
  html += ".bgrow{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-top:6px;}";
  html += ".bgrow form{display:inline;margin:0;}";
  html += ".bgrow button{margin-top:0;padding:4px 10px;font-size:13px;background:#d9534f;color:#fff;border:none;border-radius:4px;}</style></head><body>";
  html += "<h2>Desk Countdown Clock</h2>";
  if (statusMsg.length()) html += "<p class='msg'>" + statusMsg + "</p>";
  html += "<p>Current device time: <span id='curTime'>" + String(nowBuf) + "</span></p>";
  html += "<p class='status' id='powerStatus'>Power: " + String(usbPresent ? "USB" : "Battery") +
          " &middot; Input rail: " + String(inputRailVoltage, 2) + "V</p>";
  html += "<p class='status' id='displayStatus'>Display: " +
          String(backlightDimmed ? "dimmed" : "full") + "</p>";
  html += "<p class='status' id='networkStatus'>Network: " + String(networkDhcp ? "DHCP" : "Static") +
          " &middot; IP: " + WiFi.localIP().toString() + " &middot; Gateway: " + WiFi.gatewayIP().toString() +
          " &middot; Subnet: " + WiFi.subnetMask().toString() + " &middot; DNS: " + WiFi.dnsIP().toString() + "</p>";
  if (batteryFound) {
    char battBuf[80];
    snprintf(battBuf, sizeof(battBuf), "Battery: %.0f%% (%.2f V)", batteryPercent, batteryVoltage);
    html += "<p class='status' id='battStatus'>" + String(battBuf) + "</p>";
  }
  if (pmuFound) {
    String tempText = systemTempValid ? String(systemTempC, 1) + " &deg;C" :
                      (systemTempFault ? "unavailable" : "validating");
    html += "<p class='status' id='systemTempStatus'>System temperature: " + tempText + "</p>";
  }
  if (!sdReady) {
    html += "<p class='warn'>SD card not detected -- backgrounds are unavailable.</p>";
  } else if (bgLoadFailed) {
    html += "<p class='warn'>Last background failed to load -- check it's a valid JPEG. Falling back to solid black.</p>";
  }

  html += "<form method='POST' action='/save'>";
  html += "<h3>Countdown Target</h3>";
  html += "<label>Target Year</label><input type='number' name='ty' min='2024' max='2100' value='" + String(target.year) + "'>";
  html += "<label>Target Month (1-12)</label><input type='number' name='tmo' min='1' max='12' value='" + String(target.month) + "'>";
  html += "<label>Target Day (1-31)</label><input type='number' name='td' min='1' max='31' value='" + String(target.day) + "'>";
  html += "<label>Target Hour (0-23)</label><input type='number' name='th' min='0' max='23' value='" + String(target.hour) + "'>";
  html += "<label>Target Minute (0-59)</label><input type='number' name='tmi' min='0' max='59' value='" + String(target.minute) + "'>";
  html += "<label>Target Second (0-59)</label><input type='number' name='ts' min='0' max='59' value='" + String(target.second) + "'>";
  html += "<label>Countdown footer text</label><input type='text' name='eventText' maxlength='47' value='" + jsonEscape(String(countdownEventText)) + "'>";
  html += "<label>Timezone</label>";
  html += "<select id='tzPreset' onchange=\"if(this.value) document.getElementsByName('tz')[0].value=this.value;\">";
  bool tzMatched = false;
  for (int i = 0; i < TZ_PRESET_COUNT; i++) {
    bool isMatch = (String(tzString) == TZ_PRESETS[i].tz);
    if (isMatch) tzMatched = true;
    html += "<option value='" + String(TZ_PRESETS[i].tz) + "'" + (isMatch ? " selected" : "") + ">" + TZ_PRESETS[i].label + "</option>";
  }
  html += "<option value=''" + String(!tzMatched ? " selected" : "") + ">Other / Custom (enter POSIX TZ string below)</option>";
  html += "</select>";
  html += "<label>Timezone (POSIX TZ string)</label><input type='text' name='tz' value='" + String(tzString) + "'>";
  html += "<h3>IP Configuration</h3>";
  html += "<label>Address mode</label><select name='netMode'>";
  html += "<option value='dhcp'" + String(networkDhcp ? " selected" : "") + ">DHCP</option>";
  html += "<option value='static'" + String(!networkDhcp ? " selected" : "") + ">Static</option></select>";
  html += "<label>Static IP address</label><input type='text' name='netIp' value='" + networkIp.toString() + "'>";
  html += "<label>Gateway</label><input type='text' name='netGateway' value='" + networkGateway.toString() + "'>";
  html += "<label>Subnet mask</label><input type='text' name='netSubnet' value='" + networkSubnet.toString() + "'>";
  html += "<label>Primary DNS</label><input type='text' name='netDns1' value='" + networkDns1.toString() + "'>";
  html += "<label>Secondary DNS</label><input type='text' name='netDns2' value='" + networkDns2.toString() + "'>";
  html += "<p class='status'>Changing IP settings causes an automatic reboot.</p>";
  html += "<h3>Network Time (NTP)</h3>";
  html += "<label>Primary NTP server</label><input type='text' name='ntp1' maxlength='63' value='" + String(ntpServer1) + "'>";
  html += "<label>Secondary NTP server</label><input type='text' name='ntp2' maxlength='63' value='" + String(ntpServer2) + "'>";
  html += "<h3>Display / Battery</h3>";
  html += "<label>Dim brightness % (applied on battery, when idle)</label><input type='number' name='dim' min='0' max='100' value='" + String(dimPercent) + "'>";
  html += "<label>Idle seconds before dimming</label><input type='number' name='idle' min='1' max='3600' value='" + String(idleTimeoutSec) + "'>";
  html += "<label>Clock screen time format</label><select name='timefmt'>";
  html += "<option value='24'" + String(!use12HourTime ? " selected" : "") + ">24-hour</option>";
  html += "<option value='12'" + String(use12HourTime ? " selected" : "") + ">12-hour (AM/PM)</option>";
  html += "</select>";
  if (lightSensorFound) {
    html += "<label>Dim when ambient light falls below (lux)</label><input type='number' name='dimLux' min='0' step='0.1' value='" + String(dimBelowLux, 1) + "'>";
    html += "<label>Undim once ambient light rises above (lux)</label><input type='number' name='undimLux' min='0' step='0.1' value='" + String(undimAboveLux, 1) + "'>";
    html += "<p class='status' id='luxStatus'>Current ambient light: " + String(currentLux, 1) + " lux (" + String(ambientDark ? "dark -- eligible to dim" : "bright") + ")</p>";
  }
  if (pmuFound) {
    html += "<h3>System Temperature Alert</h3>";
    html += "<label>High-temperature alert</label><select name='buzzEnabled'>";
    html += "<option value='1'" + String(buzzerEnabled ? " selected" : "") + ">Enabled</option>";
    html += "<option value='0'" + String(!buzzerEnabled ? " selected" : "") + ">Disabled</option>";
    html += "</select>";
    html += "<label>Alarm when system temperature reaches (&deg;C)</label><input type='number' name='buzzTripC' step='0.5' value='" + String(buzzerTripC, 1) + "'>";
    html += "<label>Clear alarm once system temperature drops below (&deg;C)</label><input type='number' name='buzzClearC' step='0.5' value='" + String(buzzerClearC, 1) + "'>";
    String speakerStatus = buzzerAlarming ? "ALARM -- system temperature high" :
                          (!systemTempValid ? "INHIBITED -- system temperature unavailable" : "Armed / OK");
    html += "<p class='status' id='buzzStatus'>Speaker alert: " + speakerStatus + "</p>";
    html += "<p class='status'>System Temperature is measured by the onboard PCB NTC through the AXP2101 TS input. The alert state is active; audible output will use the onboard ES8311 speaker path.</p>";
  }
  html += "<button type='submit'>Save</button></form>";

  if (sdReady) {
    html += "<h3>Background</h3>";
    html += "<form method='POST' action='/selectBackground'>";
    html += "<div class='bglist'>";
    html += "<label><input type='radio' name='bgfile' value='' " + String(currentBgFile.length() == 0 ? "checked" : "") + "> None (solid black)</label>";
    std::vector<String> files = listBackgroundFiles();
    for (auto &f : files) {
      String checked = (f == currentBgFile) ? "checked" : "";
      html += "<label><input type='radio' name='bgfile' value='" + f + "' " + checked + "> " + f + "</label>";
    }
    html += "</div>";
    html += "<button type='submit'>Use Selected Background</button>";
    html += "</form>";

    if (!files.empty()) {
      html += "<h3>Manage Backgrounds</h3>";
      for (auto &f : files) {
        html += "<div class='bgrow'><span>" + f + (f == currentBgFile ? " (active)" : "") + "</span>";
        html += "<form method='POST' action='/deleteBackground' onsubmit=\"return confirm('Delete " + f + "?');\">";
        html += "<input type='hidden' name='file' value='" + f + "'>";
        html += "<button type='submit'>Delete</button>";
        html += "</form></div>";
      }

      html += "<h3>Auto-Rotate Backgrounds</h3>";
      html += "<form method='POST' action='/saveBgRotation'>";
      html += "<label>Background mode</label><select name='bgMode'>";
      html += "<option value='static'" + String(!bgRotationEnabled ? " selected" : "") + ">Static (use the selection above)</option>";
      html += "<option value='rotate'" + String(bgRotationEnabled ? " selected" : "") + ">Rotate</option>";
      html += "</select>";
      html += "<label>Rotation interval (minutes, minimum 5)</label><input type='number' name='bgRotMin' min='5' value='" + String(bgRotationIntervalMin) + "'>";
      html += "<label>Rotation pattern</label><select name='bgRotMode'>";
      html += "<option value='seq'" + String(!bgRotationShuffle ? " selected" : "") + ">Sequential</option>";
      html += "<option value='shuffle'" + String(bgRotationShuffle ? " selected" : "") + ">Shuffle</option>";
      html += "</select>";
      html += "<button type='submit'>Save Rotation Settings</button>";
      html += "</form>";
      html += "<p class='status'>Manually picking a background above still works any time -- ";
      html += "auto-rotation will just pick its own next image again at the next interval.</p>";
    }

    html += "<h3>Upload New Background</h3>";
    html += "<p class='status'>JPEG only (.jpg/.jpeg). Images close to 480x320 landscape look best -- ";
    html += "larger images are auto-scaled down and centered, with black letterboxing on the short side.</p>";
    html += "<form method='POST' action='/uploadBackground' enctype='multipart/form-data'>";
    html += "<input type='file' name='bgupload' accept='.jpg,.jpeg'>";
    html += "<button type='submit'>Upload</button>";
    html += "</form>";
  }

  html += "<h3>Diagnostic Logs</h3>";
  html += "<p class='status'>events.log contains event-only diagnostics. battery.csv contains one battery sample every 15 minutes.</p>";
  html += "<div class='button-row'>";
  html += "<a class='button-link' href='/logs/events.log' download>Download events.log</a>";
  html += "<form method='POST' action='/logs/clear' onsubmit=\"return confirm('Clear events.log? This cannot be undone.');\"><button type='submit' class='danger'>Clear event log</button></form>";
  html += "</div>";
  html += "<div class='button-row'>";
  html += "<a class='button-link' href='/logs/battery.csv' download>Download battery.csv</a>";
  html += "<form method='POST' action='/logs/battery/clear' onsubmit=\"return confirm('Clear battery.csv? This cannot be undone.');\"><button type='submit' class='danger'>Clear battery log</button></form>";
  html += "</div>";

  html += "<script>"
          "function fmtStatus(d){"
          "var e=document.getElementById('curTime'); if(e) e.textContent=d.time;"
          "e=document.getElementById('powerStatus'); if(e) e.textContent='Power: '+(d.usbPresent?'USB':'Battery')+' - Input rail: '+d.inputRailVoltage.toFixed(2)+'V';e=document.getElementById('displayStatus'); if(e) e.textContent='Display: '+(d.backlightDimmed?'dimmed':'full');e=document.getElementById('networkStatus');if(e)e.textContent='Network: '+d.networkMode+' - IP: '+d.ipAddress+' - Gateway: '+d.gateway+' - Subnet: '+d.subnet+' - DNS: '+d.dns;"
          "e=document.getElementById('battStatus'); if(e&&d.batteryFound) e.textContent='Battery: '+Math.round(d.batteryPercent)+'% ('+d.batteryVoltage.toFixed(2)+' V)';e=document.getElementById('systemTempStatus'); if(e){var ts=d.systemTempValid?(d.systemTempC.toFixed(1)+' °C'):(d.systemTempFault?'unavailable':'validating'); e.textContent='System temperature: '+ts;}"
          "e=document.getElementById('luxStatus'); if(e&&d.lightSensorFound) e.textContent='Current ambient light: '+d.currentLux.toFixed(1)+' lux ('+(d.ambientDark?'dark -- eligible to dim':'bright')+')';"
          "e=document.getElementById('buzzStatus'); if(e) e.textContent='Speaker alert: '+(d.buzzerAlarming?'ALARM -- system temperature high':(!d.systemTempValid?'INHIBITED -- system temperature unavailable':'Armed / OK'));"
          "}"
          "function pollStatus(){fetch('/status',{cache:'no-store'}).then(function(r){if(!r.ok)throw new Error('HTTP '+r.status);return r.json();}).then(fmtStatus).catch(function(err){console.error('Status update failed:',err);});}"
          "pollStatus();setInterval(pollStatus,3000);"
          "</script>";
  html += "</body></html>";
  return html;
}

String contentTypeForPath(const String &path) {
  if (path.endsWith(".html")) return "text/html";
  if (path.endsWith(".css")) return "text/css";
  if (path.endsWith(".js")) return "application/javascript";
  if (path.endsWith(".json")) return "application/json";
  if (path.endsWith(".svg")) return "image/svg+xml";
  if (path.endsWith(".png")) return "image/png";
  if (path.endsWith(".jpg") || path.endsWith(".jpeg")) return "image/jpeg";
  if (path.endsWith(".ico")) return "image/x-icon";
  if (path.endsWith(".csv")) return "text/csv";
  if (path.endsWith(".txt") || path.endsWith(".log")) return "text/plain";
  return "application/octet-stream";
}

void prepareWebResponse(bool noCache = true) {
  // The synchronous Arduino WebServer can otherwise retain an idle HTTP
  // connection long enough to starve the main loop. This device serves only
  // small control/status transactions, so close each response explicitly.
  server.sendHeader("Connection", "close");
  if (noCache) {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
  }
}

bool serveSdFile(const String &path) {
  if (!sdReady || !SD.exists(path)) return false;
  File file = SD.open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  server.sendHeader("Connection", "close");
  if (path.startsWith("/web/")) {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
  } else if (path.startsWith("/logs/")) {
    server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    server.sendHeader("Pragma", "no-cache");
    server.sendHeader("Expires", "0");
    String downloadName = path.substring(path.lastIndexOf('/') + 1);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + downloadName + "\"");
  } else {
    server.sendHeader("Cache-Control", path.endsWith(".html") ? "no-store" : "public, max-age=3600");
  }
  server.streamFile(file, contentTypeForPath(path));
  file.close();

  return true;
}

void redirectHome(const String &message = "") {
  String location = "/";
  if (message.length()) location += "?msg=" + message;
  server.sendHeader("Location", location, true);
  server.sendHeader("Connection", "close");
  server.send(303, "text/plain", "See Other");
}

void handleRoot() {
  if (serveSdFile(WEB_INDEX_PATH)) return;
  prepareWebResponse();
  server.send(200, "text/html", htmlPage("SD web assets unavailable -- using firmware fallback."));
}

void handleWebAsset() {
  String uri = server.uri();
  if (uri.indexOf("..") >= 0 || uri.indexOf('\\') >= 0) {
    prepareWebResponse();
    server.send(400, "text/plain", "Invalid path");
    return;
  }
  if (!serveSdFile(uri)) {
    prepareWebResponse();
    server.send(404, "text/plain", "Not found");
  }
}

void handleBackgroundAsset() {
  if (!server.hasArg("file")) {
    prepareWebResponse();
    server.send(400, "text/plain", "Missing background filename");
    return;
  }

  const String filename = server.arg("file");
  if (!isValidBackgroundFilename(filename)) {
    prepareWebResponse();
    server.send(400, "text/plain", "Invalid background filename");
    return;
  }

  const String path = String(BACKGROUNDS_DIR) + "/" + filename;
  if (!serveSdFile(path)) {
    prepareWebResponse();
    server.send(404, "text/plain", "Background not found");
  }
}


void handleFavicon() {
  prepareWebResponse();
  server.send(404, "text/plain", "No favicon");
}

void handleNotFound() {
  const String uri = server.uri();
  prepareWebResponse();
  server.send(404, "text/plain", "Not found: " + uri);
}

void handleConfigJson() {
  String json = "{";
  json += "\"target\":{";
  json += "\"year\":" + String(target.year) + ",";
  json += "\"month\":" + String(target.month) + ",";
  json += "\"day\":" + String(target.day) + ",";
  json += "\"hour\":" + String(target.hour) + ",";
  json += "\"minute\":" + String(target.minute) + ",";
  json += "\"second\":" + String(target.second) + "},";
  json += "\"timezone\":\"" + jsonEscape(String(tzString)) + "\",";
  json += "\"ntpServer1\":\"" + jsonEscape(String(ntpServer1)) + "\",";
  json += "\"ntpServer2\":\"" + jsonEscape(String(ntpServer2)) + "\",";
  json += "\"countdownEventText\":\"" + jsonEscape(String(countdownEventText)) + "\",";
  json += "\"network\":{";
  json += "\"hostname\":\"" + jsonEscape(String(networkHostname)) + "\",";
  json += "\"dhcp\":" + String(networkDhcp ? "true" : "false") + ",";
  json += "\"ip\":\"" + networkIp.toString() + "\",";
  json += "\"gateway\":\"" + networkGateway.toString() + "\",";
  json += "\"subnet\":\"" + networkSubnet.toString() + "\",";
  json += "\"dns1\":\"" + networkDns1.toString() + "\",";
  json += "\"dns2\":\"" + networkDns2.toString() + "\",";
  json += "\"liveIp\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"liveGateway\":\"" + WiFi.gatewayIP().toString() + "\",";
  json += "\"liveSubnet\":\"" + WiFi.subnetMask().toString() + "\",";
  json += "\"liveDns\":\"" + WiFi.dnsIP().toString() + "\"},";
  json += "\"dimPercent\":" + String(dimPercent) + ",";
  json += "\"idleTimeoutSec\":" + String(idleTimeoutSec) + ",";
  json += "\"use12HourTime\":" + String(use12HourTime ? "true" : "false") + ",";
  json += "\"dimBelowLux\":" + String(dimBelowLux, 1) + ",";
  json += "\"undimAboveLux\":" + String(undimAboveLux, 1) + ",";
  json += "\"buzzerEnabled\":" + String(buzzerEnabled ? "true" : "false") + ",";
  json += "\"speakerMuted\":" + String(speakerMuted ? "true" : "false") + ",";
  json += "\"buzzerTripC\":" + String(buzzerTripC, 1) + ",";
  json += "\"buzzerClearC\":" + String(buzzerClearC, 1) + ",";
  json += "\"sdReady\":" + String(sdReady ? "true" : "false") + ",";
  json += "\"currentBgFile\":\"" + jsonEscape(currentBgFile) + "\",";
  json += "\"bgRotationEnabled\":" + String(bgRotationEnabled ? "true" : "false") + ",";
  json += "\"bgRotationIntervalMin\":" + String(bgRotationIntervalMin) + ",";
  json += "\"bgRotationShuffle\":" + String(bgRotationShuffle ? "true" : "false");
  json += "}";
  prepareWebResponse();
  server.send(200, "application/json", json);
}

void handleBackgroundsJson() {
  String json = "{\"files\":[";
  if (sdReady) {
    std::vector<String> files = listBackgroundFiles();
    for (size_t i = 0; i < files.size(); i++) {
      if (i) json += ",";
      json += "\"" + jsonEscape(files[i]) + "\"";
    }
  }
  json += "]}";
  prepareWebResponse();
  server.send(200, "application/json", json);
}

void handleEventLogDownload() {
  if (!serveSdFile(I2C_EVENT_LOG_PATH)) server.send(404, "text/plain", "No event log available");
}

void handleBatteryLogDownload() {
  if (!serveSdFile(BATTERY_LOG_PATH)) server.send(404, "text/plain", "No battery log available yet");
}

void handleClearEventLog() {
  if (!sdReady) {
    server.send(503, "text/plain", "SD card unavailable");
    return;
  }
  SD.remove(I2C_EVENT_LOG_PATH);
  redirectHome("Event log cleared");
}

void handleClearBatteryLog() {
  if (!sdReady) {
    server.send(503, "text/plain", "SD card unavailable");
    return;
  }

  // Battery telemetry files are opened only for individual writes, so there
  // is no persistent file handle to close here. Remove both the active log
  // and any rotated copy, then recreate the active file with its CSV header.
  SD.remove(BATTERY_LOG_PATH);
  SD.remove(BATTERY_LOG_OLD_PATH);

  File logFile = SD.open(BATTERY_LOG_PATH, FILE_WRITE);
  if (!logFile) {
    server.send(500, "text/plain", "Unable to recreate battery.csv");
    return;
  }
  logFile.println("timestamp,uptime_s,power,battery_percent,battery_voltage_v,system_temp_c,system_temp_valid,input_voltage_v");
  logFile.close();

  // Allow the normal telemetry task to write a fresh baseline sample on its
  // next pass rather than waiting for the previous 15-minute interval.
  batteryLogHasSample = false;
  lastBatteryLogMs = 0;

  logEvent("BATTERY_LOG_CLEARED", "source=web");
  redirectHome("Battery log cleared");
}

// Small JSON status blob polled every few seconds by the web page's own JS
// (see the <script> in htmlPage()) so the live values update without a
// full page reload -- which would otherwise blow away anything unsaved in
// the page's forms.
void handleStatus() {
  struct tm now;
  char nowBuf[32] = "unknown";
  if (getLocalTime(&now, 200)) {
    strftime(nowBuf, sizeof(nowBuf), "%Y-%m-%d %H:%M:%S", &now);
  }

  String json = "{";
  json += "\"time\":\"" + jsonEscape(String(nowBuf)) + "\",";
  json += "\"usbPresent\":" + String(usbPresent ? "true" : "false") + ",";
  json += "\"backlightDimmed\":" + String(backlightDimmed ? "true" : "false") + ",";
  json += "\"inputRailVoltage\":" + jsonFloat(inputRailVoltage, 2) + ",";
  json += "\"batteryFound\":" + String(batteryFound ? "true" : "false") + ",";
  json += "\"batteryPercent\":" + jsonFloat(batteryPercent, 0) + ",";
  json += "\"batteryVoltage\":" + jsonFloat(batteryVoltage, 2) + ",";
  json += "\"systemTempC\":" + jsonFloat(systemTempC, 1) + ",";
  json += "\"systemTempRaw\":" + String(pmuFound ? power.getTsPinValue() : 0) + ",";
  json += "\"systemTempValid\":" + String(systemTempValid ? "true" : "false") + ",";
  json += "\"systemTempFault\":" + String(systemTempFault ? "true" : "false") + ",";
  // Compatibility aliases for existing SD-hosted pages from the earlier build.
  json += "\"batteryTempC\":" + jsonFloat(systemTempC, 1) + ",";
  json += "\"thermistorValid\":" + String(systemTempValid ? "true" : "false") + ",";
  json += "\"thermistorFault\":" + String(systemTempFault ? "true" : "false") + ",";
  json += "\"lightSensorFound\":" + String(lightSensorFound ? "true" : "false") + ",";
  json += "\"currentLux\":" + jsonFloat(currentLux, 1) + ",";
  json += "\"ambientDark\":" + String(ambientDark ? "true" : "false") + ",";
  json += "\"speakerAlertEnabled\":" + String(buzzerEnabled ? "true" : "false") + ",";
  json += "\"speakerAlertAlarming\":" + String(buzzerAlarming ? "true" : "false") + ",";
  json += "\"speakerMuted\":" + String(speakerMuted ? "true" : "false") + ",";
  json += "\"speakerReady\":" + String((speakerCodecReady && speakerFileAvailable && celebrationFileAvailable) ? "true" : "false") + ",";
  json += "\"speakerPlaying\":" + String(speakerPlaying ? "true" : "false") + ",";
  json += "\"speakerPlayback\":\"" + String(
      speakerPlaybackKind == SpeakerPlaybackKind::CountdownCelebration ? "celebration" :
      speakerPlaybackKind == SpeakerPlaybackKind::TemperatureAlert ? "temperature" : "none") + "\",";
  json += "\"celebrationReady\":" + String(celebrationFileAvailable ? "true" : "false") + ",";
  json += "\"celebrationPending\":" + String(celebrationPending ? "true" : "false") + ",";
  // Legacy aliases retained until the SD-hosted page is updated everywhere.
  json += "\"buzzerAlarming\":" + String(buzzerAlarming ? "true" : "false") + ",";
  json += "\"currentBgFile\":\"" + jsonEscape(currentBgFile) + "\",";
  json += "\"hostname\":\"" + jsonEscape(String(networkHostname)) + "\",";
  json += "\"networkMode\":\"" + String(networkDhcp ? "DHCP" : "Static") + "\",";
  json += "\"ipAddress\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"gateway\":\"" + WiFi.gatewayIP().toString() + "\",";
  json += "\"subnet\":\"" + WiFi.subnetMask().toString() + "\",";
  json += "\"dns\":\"" + WiFi.dnsIP().toString() + "\"";
  json += "}";

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send(200, "application/json", json);
}

void handleSave() {
  TargetDate t;
  t.year   = server.arg("ty").toInt();
  t.month  = server.arg("tmo").toInt();
  t.day    = server.arg("td").toInt();
  t.hour   = server.arg("th").toInt();
  t.minute = server.arg("tmi").toInt();
  t.second = server.arg("ts").toInt();
  t = sanitizeTargetDate(t);
  String tz = server.arg("tz");
  if (tz.length() == 0) tz = DEFAULT_TZ;
  String ntp1 = server.arg("ntp1");
  String ntp2 = server.arg("ntp2");
  if (ntp1.length() == 0) ntp1 = ntpServer1;
  if (ntp2.length() == 0) ntp2 = ntpServer2;
  String eventText = server.arg("eventText");

  bool newDhcp = (server.arg("netMode") != "static");
  IPAddress newIp, newGateway, newSubnet, newDns1, newDns2;
  bool networkValuesValid = newIp.fromString(server.arg("netIp")) &&
                            newGateway.fromString(server.arg("netGateway")) &&
                            newSubnet.fromString(server.arg("netSubnet")) &&
                            newDns1.fromString(server.arg("netDns1")) &&
                            newDns2.fromString(server.arg("netDns2"));
  if (!networkValuesValid) {
    server.send(400, "text/plain", "Invalid IPv4 network setting");
    return;
  }
  bool networkChanged = newDhcp != networkDhcp || newIp != networkIp ||
                        newGateway != networkGateway || newSubnet != networkSubnet ||
                        newDns1 != networkDns1 || newDns2 != networkDns2;

  int dimPct  = constrain(server.arg("dim").toInt(), 0, 100);
  int idleSec = max((int)server.arg("idle").toInt(), 1);
  bool use12h = (server.arg("timefmt") == "12");

  saveAllConfig(t, tz, dimPct, idleSec);
  saveNtpConfig(ntp1, ntp2);
  saveEventText(eventText);
  saveNetworkConfig(newDhcp, newIp, newGateway, newSubnet, newDns1, newDns2);
  saveTimeFormat(use12h);
  target = t;
  tz.toCharArray(tzString, sizeof(tzString));
  dimPercent = dimPct;
  idleTimeoutSec = idleSec;
  use12HourTime = use12h;
  configTzTime(tzString, ntpServer1, ntpServer2); // immediately apply timezone/NTP changes

  if (lightSensorFound) {
    // Only present on the form (and thus only meaningful to read) when the
    // sensor was actually detected -- otherwise these fields don't exist,
    // and blindly parsing a missing field would silently zero them out.
    float newDimLux = server.arg("dimLux").toFloat();
    float newUndimLux = server.arg("undimLux").toFloat();
    if (newUndimLux <= newDimLux) newUndimLux = newDimLux + 1.0; // keep a real hysteresis gap
    dimBelowLux = newDimLux;
    undimAboveLux = newUndimLux;
    saveLightThresholds(dimBelowLux, undimAboveLux);
  }

  if (pmuFound) {
    // Present whenever the AXP2101 PMIC and its TS channel are available.
    bool newBuzzEnabled = (server.arg("buzzEnabled") == "1");
    bool newSpeakerMuted = (server.arg("speakerMuted") == "1");
    float newTripC = server.arg("buzzTripC").toFloat();
    float newClearC = server.arg("buzzClearC").toFloat();
    if (newClearC >= newTripC) newClearC = newTripC - 1.0; // keep a real hysteresis gap
    buzzerEnabled = newBuzzEnabled;
    speakerMuted = newSpeakerMuted;
    buzzerTripC = newTripC;
    buzzerClearC = newClearC;
    saveBuzzerConfig(buzzerEnabled, speakerMuted, buzzerTripC, buzzerClearC);
  }

  if (backlightDimmed) {
    setBacklight(map(dimPercent, 0, 100, 0, 255));
  }

  for (int i = 0; i < 6; i++) lastCdVal[i] = "";
  lastCdReached = false;
  lastCdTargetStr = "";
  lastClockHour = ""; lastClockMinute = ""; lastClockSecond = ""; lastClockLine2 = ""; lastAmPmStr = "";
  lastDaysRemainingValue = ""; lastDaysRemainingTarget = "";
  if (currentScreen == SCREEN_COUNTDOWN) drawCountdownStatic();
  else if (currentScreen == SCREEN_DAYS_REMAINING) drawDaysRemainingStatic();

  if (networkChanged) {
    server.send(200, "text/html",
      "<!doctype html><meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<h2>Network settings saved</h2><p>The clock is rebooting to apply the IP configuration.</p>");
    delay(1000);
    ESP.restart();
    return;
  }

  redirectHome("Settings saved");
}

void handleSelectBackground() {
  String bgfile = server.arg("bgfile");
  if (bgfile.length() > 0 && !isValidBackgroundFilename(bgfile)) {
    server.send(400, "text/plain", "Invalid background filename");
    return;
  }
  applyBackground(bgfile);
  String msg = (bgfile.length() == 0) ? "Background cleared."
             : bgLoadFailed ? "Couldn't load that image -- reverted to solid black."
             : "Background updated.";
  redirectHome(msg);
}

void handleDeleteBackground() {
  String filename = server.arg("file");
  if (!isValidBackgroundFilename(filename)) {
    server.send(400, "text/plain", "Invalid background filename");
    return;
  }

  String path = String(BACKGROUNDS_DIR) + "/" + filename;
  bool wasActive = (filename == currentBgFile);
  bool removed = sdReady && SD.remove(path);

  if (removed && wasActive) {
    applyBackground(""); // active background just got deleted -- fall back to solid black
  }

  String msg = removed ? ("Deleted " + filename + ".") : ("Could not delete " + filename + ".");
  redirectHome(msg);
}

void handleSaveBgRotation() {
  bool enabled = (server.arg("bgMode") == "rotate");
  int intervalMin = max(server.arg("bgRotMin").toInt(), (long)MIN_BG_ROTATION_MIN);
  bool shuffle = (server.arg("bgRotMode") == "shuffle");

  saveBgRotationConfig(enabled, intervalMin, shuffle);
  bgRotationEnabled = enabled;
  bgRotationIntervalMin = intervalMin;
  bgRotationShuffle = shuffle;
  lastBgRotationMs = millis(); // restart the interval countdown from now

  redirectHome("Background settings saved");
}

File uploadFile;
bool uploadAccepted = false;
bool uploadSucceeded = false;

void handleUploadBackground() {
  // Final response after the upload body has been fully processed
  redirectHome(uploadAccepted && uploadSucceeded ? "Background uploaded" : "Upload rejected or failed");
}

void handleFileUpload() {
  HTTPUpload &upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    uploadAccepted = false;
    uploadSucceeded = false;
    if (uploadFile) uploadFile.close();

    const String filename = upload.filename;
    if (!isValidBackgroundFilename(filename)) {
      Serial.println("Rejected upload: invalid background filename");
      return;
    }
    if (!sdReady) {
      Serial.println("Rejected upload: SD card not ready");
      return;
    }

    String path = String(BACKGROUNDS_DIR) + "/" + filename;
    uploadFile = SD.open(path, FILE_WRITE);
    uploadAccepted = static_cast<bool>(uploadFile);
    if (!uploadAccepted) Serial.println("Rejected upload: could not create file");
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadAccepted && uploadFile) {
      const size_t written = uploadFile.write(upload.buf, upload.currentSize);
      if (written != upload.currentSize) uploadAccepted = false;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) uploadFile.close();
    uploadSucceeded = uploadAccepted && upload.totalSize > 0;
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    uploadAccepted = false;
    uploadSucceeded = false;
  }
}

void handleSaveHostnameAndReboot() {
  if (!server.hasArg("hostname")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing hostname\"}");
    return;
  }

  String normalized;
  String error;
  if (!normalizeHostname(server.arg("hostname"), normalized, &error)) {
    server.sendHeader("Cache-Control", "no-store");
    server.send(400, "application/json",
                String("{\"ok\":false,\"error\":\"") + jsonEscape(error) + "\"}");
    return;
  }

  saveHostname(normalized);
  logEvent("HOSTNAME_CHANGED", "hostname=" + normalized);
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json",
              String("{\"ok\":true,\"hostname\":\"") + jsonEscape(normalized) +
              "\",\"restarting\":true}");
  delay(750);
  ESP.restart();
}

void handleSpeakerMute() {
  if (!server.hasArg("muted")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing muted\"}");
    return;
  }

  speakerMuted = server.arg("muted") == "1" ||
                 server.arg("muted") == "true" ||
                 server.arg("muted") == "on";

  saveBuzzerConfig(buzzerEnabled, speakerMuted, buzzerTripC, buzzerClearC);
  if (speakerMuted) stopSpeakerPlayback();

  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json",
              String("{\"ok\":true,\"speakerMuted\":") +
              (speakerMuted ? "true" : "false") + "}");
}

void handleRebootToSetup() {
  requestSetupModeOnNextBoot();
  logEvent("SETUP_MODE_REQUEST", "source=web");
  stopSpeakerPlayback();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "application/json", "{\"ok\":true,\"restarting\":true}");
  delay(500); // allow the HTTP response to leave before restarting
  ESP.restart();
}

void setupWebServer() {
  if (MDNS.begin(networkHostname)) {
    MDNS.addService("http", "tcp", 80);
  }
  server.on("/", HTTP_GET, handleRoot);
  server.on("/web/index.html", HTTP_GET, handleRoot);
  server.on("/web/style.css", HTTP_GET, handleWebAsset);
  server.on("/web/app.js", HTTP_GET, handleWebAsset);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleConfigJson);
  server.on("/api/backgrounds", HTTP_GET, handleBackgroundsJson);
  server.on("/background", HTTP_GET, handleBackgroundAsset);
  server.on("/favicon.ico", HTTP_GET, handleFavicon);
  server.on("/logs/events.log", HTTP_GET, handleEventLogDownload);
  server.on("/logs/battery.csv", HTTP_GET, handleBatteryLogDownload);
  server.on("/logs/clear", HTTP_POST, handleClearEventLog);
  server.on("/logs/battery/clear", HTTP_POST, handleClearBatteryLog);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/network/hostname", HTTP_POST, handleSaveHostnameAndReboot);
  server.on("/speaker/mute", HTTP_POST, handleSpeakerMute);
  server.on("/system/setup-mode", HTTP_POST, handleRebootToSetup);
  server.on("/selectBackground", HTTP_POST, handleSelectBackground);
  server.on("/deleteBackground", HTTP_POST, handleDeleteBackground);
  server.on("/saveBgRotation", HTTP_POST, handleSaveBgRotation);
  server.on("/uploadBackground", HTTP_POST,
            handleUploadBackground,
            handleFileUpload);
  server.onNotFound(handleNotFound);
  server.begin();
}

// ---------------------------------------------------------------------
// Screen: Countdown
// ---------------------------------------------------------------------
// Countdown grid layout: 2 rows x 3 columns, shared column x-centers for
// both the values and their labels so alignment is guaranteed by
// construction rather than by both happening to match.
#define CD_COL1_X 80
#define CD_COL2_X 240
#define CD_COL3_X 400
#define CD_ROW1_VALUE_Y 110
#define CD_ROW1_LABEL_Y 158
#define CD_ROW2_VALUE_Y 225
#define CD_ROW2_LABEL_Y 273
#define CD_CELL_W 110   // width of the region cleared/restored around each value (24pt digit advance ~38px, allow for 2 digits)
#define CD_CELL_H 60    // height of that region (24pt DSEG7 is ~47px tall)

void drawCountdownFooter() {
  setOverlayTextColor(TFT_CYAN);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(countdownEventText, tft.width() / 2, 302, 2);
}

void drawCountdownStatic() {
  paintFullBackground();
  tft.setTextDatum(MC_DATUM);
  setOverlayTextColor(TFT_CYAN);
  tft.drawString("COUNTDOWN", tft.width() / 2, 28, 4);

  setOverlayTextColor(TFT_CYAN);
  tft.drawString("Y",  CD_COL1_X, CD_ROW1_LABEL_Y, 4);
  tft.drawString("Mo", CD_COL2_X, CD_ROW1_LABEL_Y, 4);
  tft.drawString("D",  CD_COL3_X, CD_ROW1_LABEL_Y, 4);
  tft.drawString("H",  CD_COL1_X, CD_ROW2_LABEL_Y, 4);
  tft.drawString("Mi", CD_COL2_X, CD_ROW2_LABEL_Y, 4);
  tft.drawString("S",  CD_COL3_X, CD_ROW2_LABEL_Y, 4);

  drawCountdownFooter();

  for (int i = 0; i < 6; i++) lastCdVal[i] = "";
  lastCdReached = false;
  lastCdTargetStr = "";
  lastBatteryText = "";
}

// Draws (or skips, if unchanged) a single grid value, centered on colX/rowY,
// always the DSEG7 7-segment font (24pt variant, sized for this grid) for
// visual consistency with the clock screen's time.
void renderCdCell(int colX, int rowY, const char *text, int cacheIndex) {
  if (String(text) == lastCdVal[cacheIndex]) return;
  restoreBackgroundRect(colX - CD_CELL_W / 2, rowY - CD_CELL_H / 2, CD_CELL_W, CD_CELL_H);
  setOverlayTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&DSEG7Classic_Bold24pt7b);
  tft.drawString(text, colX, rowY, GFXFF);
  tft.setFreeFont(NULL); // back to built-in numeric fonts for everything else (labels, title, etc.)
  lastCdVal[cacheIndex] = text;
}

void renderCountdown(CountdownParts &cp) {
  tft.setTextDatum(MC_DATUM);

  // Target date/time, just below the title -- kept current regardless of
  // reached state, one font step smaller than the clock screen's date line
  char targetStr[32];
  snprintf(targetStr, sizeof(targetStr), "%02d %s %04d @ %02d:%02d:%02d",
           target.day, monthAbbrev(target.month), target.year,
           target.hour, target.minute, target.second);
  if (String(targetStr) != lastCdTargetStr) {
    restoreBackgroundRect(0, 54, tft.width(), 18);
    setOverlayTextColor(TFT_CYAN);
    tft.drawString(targetStr, tft.width() / 2, 62, 2);
    lastCdTargetStr = targetStr;
  }

  if (cp.reached) {
    if (!lastCdReached) {
      restoreBackgroundRect(0, 78, tft.width(), 242);
      setOverlayTextColor(TFT_GREEN);
      tft.drawString("TARGET REACHED!", tft.width() / 2, 180, 4);
      lastCdReached = true;
    }
    return;
  }
  if (lastCdReached) {
    // coming back from a "reached" state (e.g. target date edited later) --
    // force every cell to repaint since that whole area was overwritten
    drawCountdownStatic();
  }

  char yearStr[8], monthStr[4], dayStr[4], hourStr[4], minuteStr[4], secondStr[4];
  snprintf(yearStr, sizeof(yearStr), "%d", cp.years);
  snprintf(monthStr, sizeof(monthStr), "%02d", cp.months);
  snprintf(dayStr, sizeof(dayStr), "%02d", cp.days);
  snprintf(hourStr, sizeof(hourStr), "%02d", cp.hours);
  snprintf(minuteStr, sizeof(minuteStr), "%02d", cp.minutes);
  snprintf(secondStr, sizeof(secondStr), "%02d", cp.seconds);

  renderCdCell(CD_COL1_X, CD_ROW1_VALUE_Y, yearStr, 0);
  renderCdCell(CD_COL2_X, CD_ROW1_VALUE_Y, monthStr, 1);
  renderCdCell(CD_COL3_X, CD_ROW1_VALUE_Y, dayStr, 2);
  renderCdCell(CD_COL1_X, CD_ROW2_VALUE_Y, hourStr, 3);
  renderCdCell(CD_COL2_X, CD_ROW2_VALUE_Y, minuteStr, 4);
  renderCdCell(CD_COL3_X, CD_ROW2_VALUE_Y, secondStr, 5);
}

// ---------------------------------------------------------------------
// Screen: Days Remaining
// ---------------------------------------------------------------------
// Date-only view of the same configured countdown target.  It updates only
// when the calendar day or target date changes; the configured target time
// is deliberately ignored for this screen.
#define DAYS_REMAINING_VALUE_Y 145
#define DAYS_REMAINING_VALUE_RESTORE_Y 88
#define DAYS_REMAINING_VALUE_RESTORE_H 112
#define DAYS_REMAINING_TARGET_Y 226

void drawDaysRemainingStatic() {
  paintFullBackground();
  tft.setTextDatum(MC_DATUM);

  setOverlayTextColor(TFT_CYAN);
  tft.drawString("DAYS REMAINING", tft.width() / 2, 28, 4);

  drawCountdownFooter();

  lastDaysRemainingValue = "";
  lastDaysRemainingTarget = "";
  lastBatteryText = "";
}

void renderDaysRemaining(const struct tm &now) {
  tft.setTextDatum(MC_DATUM);

  const int remaining = daysRemainingToTargetDate(now, target);
  const String remainingText = String(remaining);
  if (remainingText != lastDaysRemainingValue) {
    restoreBackgroundRect(0, DAYS_REMAINING_VALUE_RESTORE_Y,
                          tft.width(), DAYS_REMAINING_VALUE_RESTORE_H);
    setOverlayTextColor(TFT_WHITE);
    tft.setFreeFont(&DSEG7Classic_Bold40pt7b);
    tft.drawString(remainingText, tft.width() / 2, DAYS_REMAINING_VALUE_Y, GFXFF);
    tft.setFreeFont(NULL);
    lastDaysRemainingValue = remainingText;
  }

  char targetDate[40];
  snprintf(targetDate, sizeof(targetDate), "%02d %s %04d",
           target.day, monthAbbrev(target.month), target.year);
  if (String(targetDate) != lastDaysRemainingTarget) {
    restoreBackgroundRect(0, 204, tft.width(), 46);
    setOverlayTextColor(TFT_WHITE);
    tft.drawString(targetDate, tft.width() / 2, DAYS_REMAINING_TARGET_Y, 4);
    lastDaysRemainingTarget = targetDate;
  }
}

// ---------------------------------------------------------------------
// Screen: Current Date / Time
// ---------------------------------------------------------------------
// Time-of-day layout.  Hours, minutes and seconds are independently cached
// so the once-per-second update only restores/redraws the seconds field.
// The colons are static and are painted once with the screen background.
#define CLOCK_TIME_Y 130
#define CLOCK_TIME_RESTORE_Y 75
#define CLOCK_TIME_RESTORE_H 100

struct ClockTimeLayout {
  int hourX;
  int minuteX;
  int secondX;
  int colon1X;
  int colon2X;
  int pairW;
  int colonW;
  int cellW;
};

ClockTimeLayout getClockTimeLayout() {
  ClockTimeLayout l;
  tft.setFreeFont(&DSEG7Classic_Bold40pt7b);
  l.pairW = tft.textWidth("88");
  l.colonW = tft.textWidth(":");
  int totalW = (l.pairW * 3) + (l.colonW * 2);
  int left = (tft.width() - totalW) / 2;
  l.hourX = left + l.pairW / 2;
  l.colon1X = left + l.pairW + l.colonW / 2;
  l.minuteX = left + l.pairW + l.colonW + l.pairW / 2;
  l.colon2X = left + (l.pairW * 2) + l.colonW + l.colonW / 2;
  l.secondX = left + (l.pairW * 2) + (l.colonW * 2) + l.pairW / 2;
  // Keep each restore rectangle strictly inside its digit pair so it cannot
  // erase either neighboring static colon.
  l.cellW = l.pairW;
  tft.setFreeFont(NULL);
  return l;
}

void drawClockStatic() {
  paintFullBackground();
  tft.setTextDatum(MC_DATUM);

  // Draw punctuation once.  Digit pairs are rendered independently by
  // renderClock(), which prevents the whole HH:MM:SS line from flickering.
  ClockTimeLayout layout = getClockTimeLayout();
  setOverlayTextColor(TFT_WHITE);
  tft.setFreeFont(&DSEG7Classic_Bold40pt7b);
  tft.drawString(":", layout.colon1X, CLOCK_TIME_Y, GFXFF);
  tft.drawString(":", layout.colon2X, CLOCK_TIME_Y, GFXFF);
  tft.setFreeFont(NULL);

  setOverlayTextColor(TFT_DARKGREY);
  String addr = "Settings: http://" + String(networkHostname) + ".local/  (" + WiFi.localIP().toString() + ")";
  tft.drawString(addr, tft.width() / 2, tft.height() - 20, 2);

  lastClockHour = ""; lastClockMinute = ""; lastClockSecond = ""; lastClockLine2 = ""; lastAmPmStr = "";
  lastBatteryText = "";
}

void renderClockField(int xCenter, const char *value, String &cache, const ClockTimeLayout &layout) {
  if (String(value) == cache) return;

  restoreBackgroundRect(xCenter - layout.cellW / 2, CLOCK_TIME_RESTORE_Y,
                        layout.cellW, CLOCK_TIME_RESTORE_H);
  setOverlayTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  tft.setFreeFont(&DSEG7Classic_Bold40pt7b);
  tft.drawString(value, xCenter, CLOCK_TIME_Y, GFXFF);
  tft.setFreeFont(NULL);
  cache = value;
}

void renderClock(struct tm &now) {
  tft.setTextDatum(MC_DATUM);

  char hourStr[4], minuteStr[4], secondStr[4];
  strftime(hourStr, sizeof(hourStr), use12HourTime ? "%I" : "%H", &now);
  strftime(minuteStr, sizeof(minuteStr), "%M", &now);
  strftime(secondStr, sizeof(secondStr), "%S", &now);

  ClockTimeLayout layout = getClockTimeLayout();
  renderClockField(layout.hourX, hourStr, lastClockHour, layout);
  renderClockField(layout.minuteX, minuteStr, lastClockMinute, layout);
  renderClockField(layout.secondX, secondStr, lastClockSecond, layout);

  // AM/PM indicator -- drawn separately in a built-in font since DSEG7's
  // character set is digits/colon/space only, no letters. Empty string
  // (and thus hidden) in 24-hour mode.
  String ampm = use12HourTime ? (now.tm_hour < 12 ? "AM" : "PM") : "";
  if (ampm != lastAmPmStr) {
    restoreBackgroundRect(0, 177, tft.width(), 18);
    if (ampm.length() > 0) {
      setOverlayTextColor(TFT_WHITE);
      tft.drawString(ampm, tft.width() / 2, 185, 2);
    }
    lastAmPmStr = ampm;
  }

  char line2[48]; // e.g. "Monday 19 Jul 2026"
  strftime(line2, sizeof(line2), "%A %d %b %Y", &now);
  if (String(line2) != lastClockLine2) {
    restoreBackgroundRect(0, 200, tft.width(), 45);
    setOverlayTextColor(TFT_WHITE);
    tft.drawString(line2, tft.width() / 2, 220, 4);
    lastClockLine2 = line2;
  }
}

// Draws a small battery icon + percentage in the top-right corner of
// whichever screen is currently showing. Only repaints when the displayed
// value actually changes, to avoid flicker.
void drawBatteryIndicator() {
  if (!batteryFound) return;

  int pct = (int)round(constrain(batteryPercent, 0, 100));
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  String text = String(buf);
  if (text == lastBatteryText) return;
  lastBatteryText = text;

  const int areaX = 370, areaY = 4, areaW = 106, areaH = 24;
  restoreBackgroundRect(areaX, areaY, areaW, areaH);

  const int iconX = areaX, iconY = areaY + 4, iconW = 28, iconH = 14;
  uint16_t fillColor = (pct < 20) ? TFT_RED : (pct < 50 ? TFT_YELLOW : TFT_GREEN);
  tft.drawRect(iconX, iconY, iconW, iconH, TFT_WHITE);
  tft.fillRect(iconX + iconW, iconY + 4, 3, iconH - 8, TFT_WHITE); // battery nub
  int fillW = (int)((iconW - 4) * pct / 100.0);
  if (fillW > 0) {
    tft.fillRect(iconX + 2, iconY + 2, fillW, iconH - 4, fillColor);
  }

  tft.setTextDatum(TR_DATUM);
  setOverlayTextColor(TFT_WHITE);
  tft.drawString(text, areaX + areaW, areaY + 3, 2);
  tft.setTextDatum(MC_DATUM); // restore default used everywhere else
}

// ---------------------------------------------------------------------
// Touch handling
// ---------------------------------------------------------------------
// Shared by touch and the button's short-press: wakes the backlight if it
// was dimmed (without also switching screens on that same interaction), or
// advances to the next display screen if it was already awake.
void handleScreenTapOrPress() {
  registerActivity();

  if (backlightDimmed) {
    return; // this interaction's job is just to wake the backlight
  }

  if (currentScreen == SCREEN_CLOCK) {
    currentScreen = SCREEN_COUNTDOWN;
  } else if (currentScreen == SCREEN_COUNTDOWN) {
    currentScreen = SCREEN_DAYS_REMAINING;
  } else {
    currentScreen = SCREEN_CLOCK;
  }

  if (currentScreen == SCREEN_COUNTDOWN) {
    drawCountdownStatic();
  } else if (currentScreen == SCREEN_DAYS_REMAINING) {
    drawDaysRemainingStatic();
  } else {
    drawClockStatic();
  }
}

bool readFt6336Touch(uint16_t &screenX, uint16_t &screenY) {
  Wire.beginTransmission(FT6336_I2C_ADDRESS);
  Wire.write(0x02); // TD_STATUS
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  if (Wire.requestFrom(FT6336_I2C_ADDRESS, (uint8_t)5) != 5) {
    return false;
  }
  uint8_t points = Wire.read() & 0x0F;
  uint16_t rawX = ((Wire.read() & 0x0F) << 8) | Wire.read();
  uint16_t rawY = ((Wire.read() & 0x0F) << 8) | Wire.read();
  if (points == 0 || rawX >= 320 || rawY >= 480) {
    return false;
  }

  // TFT rotation 1: native portrait (320x480) -> landscape (480x320).
  // This is the 180-degree counterpart of the previous rotation-3 mapping.
  screenX = 479 - rawY;
  screenY = rawX;
  return true;
}

void handleTouch() {
  uint16_t x, y;
  if (!readFt6336Touch(x, y)) return;

  unsigned long nowMs = millis();
  if (nowMs - lastTouchMs < TOUCH_DEBOUNCE_MS) return;
  lastTouchMs = nowMs;
  handleScreenTapOrPress();
}

// ---------------------------------------------------------------------
// Button (post-setup)
//   Short press: switch screens
//   Hold >=3 seconds and release before 8 seconds: normal reboot
//   Hold >=8 seconds: set one-shot flag and reboot into Setup Mode
// ---------------------------------------------------------------------
#define BUTTON_DEBOUNCE_MS       30
#define BUTTON_REBOOT_HOLD_MS  3000
#define BUTTON_SETUP_HOLD_MS   8000

bool buttonWasDown = false;
unsigned long buttonDownStartMs = 0;
bool buttonRebootPromptShown = false;
bool buttonSetupActionFired = false;

void handleButton() {
  const bool buttonDown = (digitalRead(BUTTON_PIN) == LOW);
  const unsigned long nowMs = millis();

  if (buttonDown && !buttonWasDown) {
    buttonDownStartMs = nowMs;
    buttonRebootPromptShown = false;
    buttonSetupActionFired = false;
  } else if (buttonDown && buttonWasDown) {
    const unsigned long heldMs = nowMs - buttonDownStartMs;

    if (!buttonRebootPromptShown && heldMs >= BUTTON_REBOOT_HOLD_MS) {
      buttonRebootPromptShown = true;
      showMessage("Release: Reboot", "Keep holding for Setup Mode");
    }

    if (!buttonSetupActionFired && heldMs >= BUTTON_SETUP_HOLD_MS) {
      buttonSetupActionFired = true;
      requestSetupModeOnNextBoot();
      logEvent("SETUP_MODE_REQUEST", "source=GPIO0");
      stopSpeakerPlayback();
      showMessage("Setup Mode", "Rebooting...");
      delay(500);
      ESP.restart();
    }
  } else if (!buttonDown && buttonWasDown) {
    const unsigned long heldMs = nowMs - buttonDownStartMs;

    if (!buttonSetupActionFired && heldMs >= BUTTON_REBOOT_HOLD_MS) {
      showMessage("Rebooting...");
      delay(300);
      ESP.restart();
    } else if (!buttonSetupActionFired && heldMs >= BUTTON_DEBOUNCE_MS) {
      handleScreenTapOrPress();
    }
  }

  buttonWasDown = buttonDown;
}

// ---------------------------------------------------------------------
// Setup / Loop
// ---------------------------------------------------------------------

constexpr uint8_t TCA9554_ADDRESS = 0x20;
constexpr uint8_t TCA9554_LCD_RESET_BIT = 1;
constexpr uint8_t TCA9554_SPEAKER_PA_BIT = 7;

bool tca9554WriteRegister(uint8_t reg, uint8_t value) {
  Wire.beginTransmission(TCA9554_ADDRESS);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission(true) == 0;
}

bool tca9554ReadRegister(uint8_t reg, uint8_t &value) {
  Wire.beginTransmission(TCA9554_ADDRESS);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom((int)TCA9554_ADDRESS, 1, true) != 1) return false;
  value = Wire.read();
  return true;
}

bool setSpeakerAmplifier(bool enabled) {
  uint8_t config = 0xFF;
  uint8_t output = 0xFF;
  if (!tca9554ReadRegister(0x03, config)) return false;
  if (!tca9554ReadRegister(0x01, output)) return false;
  config &= ~(1U << TCA9554_SPEAKER_PA_BIT); // output
  if (enabled) output |= (1U << TCA9554_SPEAKER_PA_BIT);
  else output &= ~(1U << TCA9554_SPEAKER_PA_BIT);
  return tca9554WriteRegister(0x03, config) &&
         tca9554WriteRegister(0x01, output);
}

void resetOnboardLcd() {
  // TCA9554 registers: output=0x01, configuration=0x03 (1=input, 0=output).
  uint8_t out = 0xFF;
  tca9554WriteRegister(0x03, (uint8_t)~(1U << TCA9554_LCD_RESET_BIT));
  tca9554WriteRegister(0x01, out);
  delay(10);
  out &= ~(1U << TCA9554_LCD_RESET_BIT);
  tca9554WriteRegister(0x01, out);
  delay(10);
  out |= (1U << TCA9554_LCD_RESET_BIT);
  tca9554WriteRegister(0x01, out);
  delay(200);
}

void setup() {
  Serial.begin(115200);
  delay(300); // give the USB-serial bridge a moment before the first prints
  Serial.println("[boot] Serial up");

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("[boot] pins configured");

  backlightInit();
  Serial.println("[boot] backlight init done");

  bool i2cPrepared = prepareI2cBus();
  Serial.printf("[boot] I2C bus prepared=%d\n", i2cPrepared ? 1 : 0);

  if (i2cPrepared) {
    // Initialize the AXP2101 first because XPowersLib opens/reconfigures Wire.
    // Initialize the VEML7700 afterward so its Adafruit_I2CDevice remains bound
    // to the final shared-bus state instead of a bus subsequently restarted by
    // the PMU driver.
    batteryInit();
    updateUsbPresence(true);
    Serial.println("[boot] AXP2101 init done");

    lightSensorInit();
    Serial.println("[boot] light sensor init done");
  } else {
    lightSensorFound = false;
    batteryFound = false;
    systemTempValid = false;
    systemTempFault = true;
    Serial.println("[boot] I2C sensor initialization skipped");
  }

  Serial.println("[boot] temperature alert state initialized");
  batteryI2cExpected = pmuFound;
  lightI2cExpected = lightSensorFound;

  resetOnboardLcd();
  Serial.println("[boot] onboard LCD reset done");
  tft.init();
  Serial.println("[boot] tft.init() done");
  tft.setRotation(1); // landscape rotated 180 degrees from the previous rotation-3 orientation
  Serial.println("[boot] tft.setRotation() done");
  tft.invertDisplay(true); // ST7796S panel requires inversion for correct black/white and RGB colors
  Serial.println("[boot] tft.invertDisplay(true) done");

  sdInit(); // onboard 1-bit SD_MMC interface
  Serial.println("[boot] sdInit() done");
  speakerInit();
  Serial.println("[boot] ES8311 speaker init done");
  logEvent("BOOT_I2C_STATUS",
           "bus=" + String(i2cBusReady ? 1 : 0) +
           ",pmu=" + String(pmuFound ? 1 : 0) +
           ",battery=" + String(batteryFound ? 1 : 0) +
           ",light=" + String(lightSensorFound ? 1 : 0));

  // TJpg_Decoder and TFT_eSPI's pushImage() have their own conventions for
  // RGB565 byte order; without this they disagree and colors come out
  // scrambled/wrong (not just tinted -- channels effectively swap, giving a
  // "very colorized"/wrong-hue look). This only needs setting once.
  TJpgDec.setSwapBytes(true);

  Serial.println("[boot] FT6336 capacitive touch ready");

  delay(50); // let the button pin settle
  const bool buttonHeld = (digitalRead(BUTTON_PIN) == LOW);
  const bool forceSetupRequested = consumeSetupModeRequest();
  const bool needsFirstTimeSetup = !hasBeenConfigured();
  Serial.printf("[boot] buttonHeld=%d forceSetupRequested=%d needsFirstTimeSetup=%d\n",
                buttonHeld, forceSetupRequested, needsFirstTimeSetup);

  if (buttonHeld || forceSetupRequested || needsFirstTimeSetup) {
    Serial.println("[boot] entering forced config portal");
    runConfigPortal(true); // returns only on success; reboots otherwise
  } else {
    showMessage("Connecting WiFi...");
    Serial.println("[boot] attempting normal WiFi autoConnect");
    runConfigPortal(false); // autoConnect: uses saved creds, falls back to portal if needed
  }
  Serial.println("[boot] WiFi connected");

  loadConfig();
  Serial.println("[boot] loadConfig() done");

  if (currentBgFile.length() > 0) {
    Serial.printf("[boot] loading background: %s\n", currentBgFile.c_str());
    bgLoadFailed = !loadBackgroundFromSD(currentBgFile);
    if (bgLoadFailed) currentBgFile = "";
    Serial.println("[boot] background load attempt done");
  }

  showMessage("Syncing time...");
  configTzTime(tzString, ntpServer1, ntpServer2);
  Serial.printf("[boot] NTP primary=%s secondary=%s\n", ntpServer1, ntpServer2);
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo, 5000) && attempts < 20) attempts++;
  Serial.printf("[boot] NTP sync attempts=%d\n", attempts);

  setupWebServer();
  Serial.println("[boot] web server started");

  registerActivity(); // start at full brightness
  drawCountdownStatic();
  Serial.println("[boot] first screen drawn -- entering loop()");
  currentScreen = SCREEN_COUNTDOWN;
}

unsigned long lastTick = 0;
unsigned long lastResync = 0;
unsigned long lastBacklightCheck = 0;

void loop() {

  if (WiFi.status() != WL_CONNECTED &&
      millis() - lastWifiReconnectAttemptMs >= WIFI_RECONNECT_INTERVAL_MS) {
    lastWifiReconnectAttemptMs = millis();
    WiFi.reconnect();
  }

  server.handleClient();
  handleTouch();
  handleButton();
  rotateBackgroundIfDue();
  updateBuzzer();
  serviceSpeakerAudio();

  if (millis() - lastBacklightCheck >= 250) {
    lastBacklightCheck = millis();
    updateBacklight();
  }

  if (millis() - lastResync > 6UL * 60UL * 60UL * 1000UL) {
    configTzTime(tzString, ntpServer1, ntpServer2);
    lastResync = millis();
  }

  if (millis() - lastTick >= 1000) {
    lastTick = millis();
    struct tm now;
    if (getLocalTime(&now, 200)) {
      CountdownParts cp = calendarDiff(now, target);
      updateCountdownCelebration(cp.reached);
      if (currentScreen == SCREEN_COUNTDOWN) {
        renderCountdown(cp);
      } else if (currentScreen == SCREEN_DAYS_REMAINING) {
        renderDaysRemaining(now);
      } else {
        renderClock(now);
      }
    }
    updateBattery();
    logBatteryTelemetry();
    drawBatteryIndicator();
  }

  serviceI2cRecovery();
}
