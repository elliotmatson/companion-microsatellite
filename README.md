# Companion µSatellite

A PlatformIO/Arduino library that connects ESP32 microcontrollers to [Bitfocus Companion](https://bitfocus.io/companion) via the [Satellite API](https://companion.free/for-developers/Satellite-API), turning a microcontroller into a virtual Stream Deck-style surface.

## Features

- TCP connection to Companion with automatic reconnection
- REST API for configuration via a web interface
- mDNS service discovery
- Multiple virtual surfaces, plus standalone button subscriptions
- Button press/release, rotary encoder, and page-navigation support
- Variable input/output support with base64 encoding
- Pincode input support
- Bitmap format negotiation (rgb/png/webp)
- Custom config fields with runtime updates via DEVICE-CONFIG
- Thread-safe operations with mutex protection
- Persistent configuration storage

Requires Companion Satellite API version 1.10.0 or later (Companion v4.3+).

## Installation

**PlatformIO:** add this repository to your project's `platformio.ini`:

```ini
lib_deps =
    https://github.com/elliotmatson/companion-microsatellite.git
```

The library declares its own dependencies (ESPAsyncWebServer, AsyncTCP, ArduinoJson), so PlatformIO will pull those in automatically.

**Arduino IDE:** clone or download this repository into your `Arduino/libraries/` folder (or use *Sketch > Include Library > Add .ZIP Library*), then install ESPAsyncWebServer, AsyncTCP, and ArduinoJson yourself via the Library Manager, since Arduino IDE doesn't resolve a library's own dependencies automatically the way PlatformIO does.

## Usage

```cpp
#include "companion-satellite.hpp"

Satellite satellite("my-device");

SatelliteSurfaceConfig config = {
    .totalKeys = 4,
    .keysPerRow = 2,
    .sendColors = true,
    .sendText = true,
};

SatelliteSurface surface(satellite, "Surface 1", config);

void setup() {
    // ... connect to WiFi first ...
    satellite.begin();

    surface.setOnKeyStateChange([](const SatelliteSurfaceKeyState &state) {
        // update an LED, display, etc. based on state.keyColor / state.text
    });
}

void loop() {
    // surface.buttonAction(0, SatelliteAction::PRESSED) to report input
}
```

## Examples

Arduino IDE sketches (`.ino`, no `platformio.ini`) - open directly in the Arduino IDE, or compile with `arduino-cli`:

- [`examples/basic`](examples/basic) - WiFi, a NeoPixel driven by key color, and a physical button reporting presses back to Companion.
- [`examples/subscriptions`](examples/subscriptions) - observes and interacts with a single button by absolute location using the button *subscription* API, instead of registering a full surface. **Button subscriptions must be enabled for the connection in Companion's settings** before this will work - the example checks `supportsSubscriptions()` and logs a clear warning if it isn't.
- [`examples/configuration-and-variables`](examples/configuration-and-variables) - a custom settings form (`CONFIG_FIELDS`/`DEVICE-CONFIG`, Companion -> device) alongside an input variable and an output variable (`setVariable()`/`setOnVariableSet()`, both directions).
- [`examples/pincode`](examples/pincode) - a surface that handles Companion's pincode-lock feature itself: shows the locked state and forwards digit presses with `pressPincodeKey()`.
- [`examples/cyd`](examples/cyd) - renders a 4x3 button surface directly on a "Cheap Yellow Display" (ESP32-2432S028R) touchscreen, decoding and drawing the actual bitmap Companion sends for each key, and reporting touches as button presses. Needs the extra `TFT_eSPI` and `XPT2046_Touchscreen` libraries; targets that one board rather than building across variants.

PlatformIO projects are prefixed `pio-` and have their own `platformio.ini`:

- [`examples/pio-exhaustive`](examples/pio-exhaustive) - runs through every public method and callback on `Satellite` and `SatelliteSurface` once at boot (surfaces, subscriptions, variables, pincode, page navigation, firmware-update notification, and more). No hardware beyond WiFi required, so it builds and runs the same on any variant.

All examples except `cyd` are compiled in CI across `esp32`, `esp32s2`, `esp32s3`, `esp32c3`, and `esp32c6` (see `.github/workflows/build-examples.yml`) - the `.ino` sketches via `arduino-cli`, `pio-exhaustive` via `pio run`. `cyd` is compiled for `esp32` only, since it targets one specific board.

## License

MIT, see [LICENSE](LICENSE).
