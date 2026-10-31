# keypad-3x3

A 9-key Companion surface: nine push buttons in a 3×3 grid, each with an APA102 ("DotStar") LED showing the key's color. WiFi is set up from a phone, and until Companion has the surface the LEDs show where it's got to.

## Hardware

Written for an ESP32-C3 board. Some of the pins don't exist on other ESP32 variants, so change them at the top of the sketch to match your wiring.

| Part                                  | Pins                                               |
| ------------------------------------- | -------------------------------------------------- |
| Buttons, left to right, top to bottom | GPIO 1, 0, 5, 6, 7, 9, 10, 20, 21 (each to ground) |
| 9 × APA102/DotStar LEDs               | data: GPIO 3, clock: GPIO 4                        |

The LED chain runs right to left along each row. If yours runs left to right, change `kLedForKey` to `{0, 1, 2, 3, 4, 5, 6, 7, 8}`.

GPIO 9 is the C3's BOOT strapping pin: holding that key while the board resets starts the bootloader instead of the sketch.

## Libraries

Install with the Arduino Library Manager: ESP Async WebServer and Async TCP (both by ESP32Async), ArduinoJson, Adafruit DotStar and WiFiManager (tzapu, 2.0 or later), plus this library.

## Setup

1. Flash it, with the partition scheme set to "Huge APP (3MB No OTA/1MB SPIFFS)" (Arduino IDE: Tools > Partition Scheme; PlatformIO: `board_build.partitions = huge_app.csv`). The sketch is too big for the default one. With no WiFi network saved, it starts a WiFi network of its own called `Keypad-Companion-XXXX`, and every key blinks blue.
2. Join that network from a phone. The setup page should open by itself; if not, browse to `http://192.168.4.1`.
3. Pick your WiFi network and enter its password. Optionally, enter Companion's address (`192.168.1.10`, or `host:port` if it isn't on 16622) - or leave it empty and add the device from Companion's Surfaces page, where it's discovered automatically.
4. "3x3 Keypad" appears in Companion. Set a background color on a button in Companion and its LED follows; the surface's brightness setting dims them all.

If the saved network can't be reached when it starts (eg. after a power cut, before the router is back), it opens the setup network for 5 minutes, then restarts and tries again.

To forget the WiFi network and set up another, hold the top-left key while the board starts (plug it in, or press reset). Every key blinks white; keep holding for 3 seconds. Let go before then to cancel.

## Status lights

Until Companion has the surface, the LEDs show where it's got to:

| LEDs                      | Meaning                                                    |
| ------------------------- | ---------------------------------------------------------- |
| Every key blue, blinking  | WiFi setup: join the `Keypad-Companion-XXXX` network       |
| Middle key blue           | Joining the WiFi network                                   |
| Middle key cyan, blinking | On WiFi, looking for Companion                             |
| Middle key red, blinking  | Companion is too old for this library (needs 4.3 or later) |
| Every key white, blinking | Top-left key held: let go to cancel forgetting WiFi        |

Once Companion has it, the LEDs show Companion's colors.

## How it works

- **Colors only:** the surface asks for key colors but not text or bitmaps, so Companion sends very little.
- **Buttons:** `loop()` polls them, ignores contact bounce, and sends a press or release only when one changes.
- **WiFi:** power saving is turned off, so color changes from Companion arrive straight away. Transmit power is lowered to 13dBm, which many small C3 boards with poorly matched antennas need to connect reliably.
