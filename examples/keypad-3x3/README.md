# keypad-3x3

A 9-key Companion surface: nine push buttons in a 3×3 grid, each with an APA102 ("DotStar") LED showing the key's color.

## Hardware

Written for an ESP32-C3 board. Some of the pins don't exist on other ESP32 variants, so change them at the top of the sketch to match your wiring.

| Part                                  | Pins                                               |
| ------------------------------------- | -------------------------------------------------- |
| Buttons, left to right, top to bottom | GPIO 1, 0, 5, 6, 7, 9, 10, 20, 21 (each to ground) |
| 9 × APA102/DotStar LEDs               | data: GPIO 3, clock: GPIO 4                        |

The LED chain runs right to left along each row. If yours runs left to right, change `kLedForKey` to `{0, 1, 2, 3, 4, 5, 6, 7, 8}`.

GPIO 9 is the C3's BOOT strapping pin: holding that key while the board resets starts the bootloader instead of the sketch.

## Libraries

Install with the Arduino Library Manager: ESP Async WebServer and Async TCP (both by ESP32Async), ArduinoJson and Adafruit DotStar, plus this library.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of `keypad-3x3.ino`. Keep real credentials out of version control.
2. Flash it and open the serial monitor at 115200 baud.
3. Point it at Companion: let Companion discover it, or set the target over the REST API on port 9999 - see [Usage](../../README.md#usage).

The surface shows up in Companion as "3x3 Keypad". Set a background color on a button in Companion and its LED follows; the surface's brightness setting dims them all.

## How it works

- **Colors only:** the surface asks for key colors but not text or bitmaps, so Companion sends very little.
- **Buttons:** `loop()` polls them, ignores contact bounce, and sends a press or release only when one changes.
- **Dark while disconnected:** the LEDs turn off when the connection to Companion drops, rather than showing stale colors. Companion sends every key's color again after reconnecting.
- **WiFi:** power saving is turned off, so color changes from Companion arrive straight away. Transmit power is lowered to 13dBm, which many small C3 boards with poorly matched antennas need to connect reliably.
