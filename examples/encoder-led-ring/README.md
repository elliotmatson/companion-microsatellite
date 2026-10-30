# encoder-led-ring

A rotary encoder surrounded by a 24-LED ring, and a separate button with a status LED, described to Companion with a **layout manifest** instead of a grid of identical keys. Each control only asks for what it can show.

## Hardware

| Part | Pins |
| --- | --- |
| Rotary encoder | A: GPIO 32, B: GPIO 33, push switch: GPIO 25 |
| 24 × WS2812/NeoPixel ring around the encoder | data: GPIO 18 |
| Push button | GPIO 26 |
| Single NeoPixel for the button | data: GPIO 19 |

Buttons and the encoder switch connect to ground (internal pull-ups).

## Libraries

ESPAsyncWebServer and AsyncTCP (both by ESP32Async), ArduinoJson and Adafruit NeoPixel, plus this library.

## What it shows

- **Layout manifest:** two controls, `0/0` (the encoder, style preset `encoder`) and `0/1` (the button, the default preset). The button only asks for colors, so there's very little data to receive.
- **LED rings** (Satellite API 1.13, Companion 4.3+): the encoder's preset asks for 24 LED segments as a full ring. Give that button an LED ring style in Companion, for example a gauge bound to a volume variable, and the ring follows it.
- **Rotation amounts** (API 1.14): encoder steps are counted in an interrupt and sent from `loop()` as one rotation of N steps, so a fast spin is a single message. With older Companion versions the library sends one message per step.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of the sketch, and change the pin numbers if your wiring differs.
2. Flash it, open the serial monitor at 115200 baud, and point it at Companion (see [Usage](../../README.md#usage)).
3. The surface appears as "Encoder + LED ring". Assign actions to the encoder's rotate left/right and press, and to the button.

The decoder assumes 4 quadrature transitions per detent, which suits most encoders. If yours counts double or half, change the `4` in `onEncoderChange()`.
