# pincode

A surface that handles Companion's pincode lock itself: it is told when a pincode-protected page locks and unlocks, and sends the digits back with `pressPincodeKey()`.

No hardware beyond the ESP32 is needed. To keep it self-contained, it "types" digits on its own while locked, so you can watch the whole exchange in the serial monitor.

## Pincode support levels

`SatelliteSurfaceConfig::pincodeSupport` says what the device can do:

- **`FULL`** (this example): it shows the locked state, and the user can enter the pincode on it.
- **`PARTIAL`**: it shows the locked state, but has no way to enter a pincode (eg. no keypad), so Companion expects it to be entered elsewhere.

Between `locked=1` and the matching `locked=0`, Companion doesn't draw anything else on the surface or accept other input from it, so the example ignores key state updates while locked.

## Libraries

ESPAsyncWebServer and AsyncTCP (both by ESP32Async) and ArduinoJson, plus this library.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of the sketch.
2. Flash it, open the serial monitor at 115200 baud, and point it at Companion (see [Usage](../../README.md#usage)).
3. In Companion, turn on the pincode lock and lock this surface (or wait for its timeout).

While locked, it presses digits 1, 2, 3, 4, 1, ... one per second. You'll see `onLockedStateSet: locked=1 charactersEntered=N rotation=...` as they're counted. Set the pincode to 1234 and the surface unlocks after four digits.

`rotation` is the surface rotation configured in Companion, so a real display can draw its lock screen the right way up.
