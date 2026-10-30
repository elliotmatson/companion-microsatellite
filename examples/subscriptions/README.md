# subscriptions

Follows and presses one existing Companion button by its location, using `SatelliteSubscription` instead of registering a whole surface. The device doesn't appear in Companion's Surfaces list; it just watches, and presses, the button at page 1, row 0, column 0.

## Turn on subscriptions in Companion first

Button subscriptions are an optional Companion setting that the device can't turn on itself. Enable them in Companion's settings for Satellite connections. Otherwise the example logs "Companion has button subscriptions disabled".

## Hardware

- Any WiFi-capable ESP32
- A push button from GPIO 0 to ground (the BOOT button on most dev boards)

## Libraries

ESPAsyncWebServer and AsyncTCP (both by ESP32Async) and ArduinoJson, plus this library.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of the sketch.
2. Change `"1/0/0"` to the location (`page/row/column`) of a button that exists in your Companion setup.
3. Flash it, open the serial monitor at 115200 baud, and point it at Companion (see [Usage](../../README.md#usage)).

Once subscribed, it logs the button's state (pressed, text and color) whenever it changes in Companion, and pressing the physical button presses the Companion button. A location that doesn't exist is reported as `Subscription rejected: ...`.

Subscriptions are registered again automatically after every reconnect: create the `SatelliteSubscription` once and keep it alive.
