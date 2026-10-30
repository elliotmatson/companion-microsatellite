# basic

A one-key Companion surface backed by real hardware: a NeoPixel shows the key's color and brightness, and a push button sends presses back to Companion.

## Hardware

- Any WiFi-capable ESP32 (CI builds it for ESP32, S2, S3, C3 and C6)
- A WS2812/NeoPixel LED, data on GPIO 18
- A push button from GPIO 0 to ground (on most dev boards this is the BOOT button, so no wiring is needed)

## Libraries

Install with the Arduino Library Manager: ESPAsyncWebServer and AsyncTCP (both by ESP32Async), ArduinoJson and Adafruit NeoPixel, plus this library.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of `basic.ino`. Keep real credentials out of version control.
2. Flash it and open the serial monitor at 115200 baud.
3. Point it at Companion: let Companion discover it, or set the target over the REST API on port 9999 - see [Usage](../../README.md#usage).

The surface shows up in Companion as "Surface 1". Set a color on its button in Companion and the NeoPixel follows it; the surface's brightness setting dims it.

## How it works

- `setOnKeyStateChange()` copies the key color to the NeoPixel, and `setOnBrightnessSet()` maps Companion's 0-100 brightness onto it.
- The button's GPIO interrupt only wakes a small FreeRTOS task, which calls `buttonAction()`. That call takes a lock, allocates and writes to the socket, none of which is safe in an interrupt handler.
- `satellite.loop()` in `loop()` runs the callbacks.
