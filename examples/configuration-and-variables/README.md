# configuration-and-variables

The two ways data moves between a device and Companion besides button presses:

- **Config fields** (`CONFIG_FIELDS` / `DEVICE-CONFIG`): a small settings form the device describes and Companion shows in the surface's settings. Companion sends the values to the device after it registers, and again whenever someone edits them.
- **Variables**: named values attached to the surface. An _input_ variable is produced by the device (`setVariable()`), an _output_ variable by Companion (`setOnVariableSet()`).

No hardware beyond the ESP32 is needed. Everything it does shows up in the serial monitor.

## What it does

- Defines two config fields: "Device Label" (text) and "Uptime report interval (seconds)" (a number from 1 to 3600, default 10).
- Reports its uptime in seconds through the input variable `uptime_seconds`, at the configured interval.
- Logs whatever Companion writes to the output variable `greeting`.

## Libraries

ESPAsyncWebServer and AsyncTCP (both by ESP32Async) and ArduinoJson, plus this library.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of the sketch.
2. Flash it, open the serial monitor at 115200 baud, and point it at Companion (see [Usage](../../README.md#usage)).
3. In Companion's surface settings for this device, change the fields: the device logs `onDeviceConfig: {...}` and the parsed values, and the uptime reports follow the new interval.
4. Use the `uptime_seconds` variable in a button, and set `greeting` from a button action to see `onVariableSet: greeting = ...`.

## Notes

- The config field JSON is an ordinary escaped string rather than a raw string literal (`R"(...)"`), because the Arduino IDE's prototype generator for `.ino` files gets confused by raw strings. In a `.cpp` file either is fine.
- Companion's `satellite-config-fields.schema.json` lists every field type (text input, number, checkbox and so on).
