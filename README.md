# Companion µSatellite

A PlatformIO/Arduino library that connects ESP32 microcontrollers to [Bitfocus Companion](https://bitfocus.io/companion) via the [Satellite API](https://companion.free/for-developers/Satellite-API), turning a microcontroller into a virtual Stream Deck-style surface.

## Features

- Virtual surfaces: a simple grid of keys, or a **layout manifest** describing each control individually (different bitmap sizes, color-only LED buttons, encoders with **addressable LED rings**)
- Button press/release, rotary encoders (multi-step rotations in one message where Companion supports it), and page navigation
- Button **subscriptions**: follow and press any button by its page/row/column location
- Variables in both directions, custom config fields, pincode lock handling, firmware update notification
- Bitmap format negotiation (rgb/png/webp), with zero-copy access to received bitmaps
- Robust connection handling: waits for `CAPS` before registering, re-registers surfaces and subscriptions after every reconnect, detects a silent Companion via keepalive, backs off between retries, and never truncates outgoing messages
- Callbacks run on your `loop()` task by default, with repeated key updates merged, so display/LED code needs no locking
- REST config API and mDNS advertisement, so Companion can discover and configure the device (both optional), with settings saved to flash

Requires Companion Satellite API version 1.10.0 or later (Companion v4.3+). LED rings need API 1.13.0 and multi-step rotations 1.14.0; older versions fall back gracefully.

## Installation

**PlatformIO:** add this repository to your project's `platformio.ini`:

```ini
lib_deps =
    https://github.com/elliotmatson/companion-microsatellite.git
```

The library declares its own dependencies (ESPAsyncWebServer, AsyncTCP, ArduinoJson), so PlatformIO will pull those in automatically.

**Arduino IDE:** clone or download this repository into your `Arduino/libraries/` folder (or use _Sketch > Include Library > Add .ZIP Library_), then install ESPAsyncWebServer, AsyncTCP, and ArduinoJson yourself via the Library Manager, since Arduino IDE doesn't resolve a library's own dependencies automatically the way PlatformIO does.

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

    surface.setOnKeyStateChange([](const SatelliteControlState &state) {
        // update an LED, display, etc. based on state.keyColor / state.text / state.bitmap
    });

    satellite.begin();
}

void loop() {
    satellite.loop(); // callbacks run in here
    // surface.buttonAction(0, SatelliteAction::PRESSED) to report input
}
```

Point the device at Companion either from Companion's own surface discovery (it finds the device over mDNS and configures it through the REST API), by POSTing `{"host": "...", "port": 16622}` to `http://<device>:9999/api/config`, or in code with `satellite.setTarget("192.168.1.10")`.

### Threading

By default (`SatelliteDispatchMode::LOOP`) every callback runs inside `satellite.loop()`, on whichever task calls it. Calling methods like `buttonAction()` or `setVariable()` is safe from any task.

Received updates wait in a bounded queue until `loop()` runs, and newer state for a key replaces older state that hasn't been delivered yet, so a slow `loop()` (eg. one drawing to a display) only ever draws the latest state.

Pass `SatelliteOptions{.dispatch = SatelliteDispatchMode::IMMEDIATE}` to `begin()` to run callbacks straight away on the network task instead - lower latency, but keep them short and thread-safe.

Bitmap and LED data (`SatelliteEncodedData`) point into the received message and are only valid until the callback returns - decode them inside it, with `decode()`.

### Layouts and LED rings

Instead of `totalKeys`/`keysPerRow`, describe each control with `config.layout`. Each control picks a style preset that says what it receives, so an LED-only button never has to buffer a bitmap:

```cpp
SatelliteSurfaceConfig config = {
    .layout = SatelliteLayout{
        .stylePresets = {
            {.name = "default", .colors = true},
            {.name = "encoder", .colors = true, .ledSegments = 24, .ledsMode = SatelliteLedsMode::FULL_RING},
            {.name = "lcd", .bitmapWidth = 128, .bitmapHeight = 64, .text = true}, // non-square needs Companion's NONSQUARE
        },
        .controls = {
            {.id = "0/0", .row = 0, .column = 0, .stylePreset = "encoder"},
            {.id = "0/1", .row = 0, .column = 1},
            {.id = "1/0", .row = 1, .column = 0, .stylePreset = "lcd"},
        },
    },
};
```

Key state for a layout surface carries `state.controlId`, and `state.index` is the control's position in `controls`; `buttonAction()`/`rotate()` take that index, or use `controlAction()`/`rotateControl()` with the id. LED colors arrive in `state.leds` as one R,G,B triple per segment.

### Memory

A raw rgb bitmap of W×H pixels arrives as a single ~W×H×4 byte line, which must fit in `SatelliteOptions::maxLineLength` (32KB by default - enough for 80×80). Larger lines are skipped with a warning rather than dropping the connection. For big keys, ask for `bitmapFormat = "png"` or `"webp"` (if you have a decoder), or a smaller size.

Received messages wait in a queue for `loop()`, up to `SatelliteOptions::maxQueuedBytes` (64KB by default). When `loop()` falls behind, receiving pauses with TCP flow control - Companion waits - rather than updates being dropped. To keep that headroom, the queue pauses once it's within `maxLineLength` plus a TCP window (5760 bytes on the ESP32) of the limit, so with the defaults that's after about 24KB. If your bitmaps are big, set `maxLineLength` to just fit them and give `maxQueuedBytes` room for a few, so that receiving and drawing can overlap ([`examples/cyd`](examples/cyd) does this for 80×80 rgb).

## Migrating from 0.0.1

0.1.0 changes some of the API. Most sketches only need the first two changes.

**Call `satellite.loop()`.** Callbacks used to run on the AsyncTCP network task as messages arrived. They now run inside `satellite.loop()`, so call it from your `loop()`. A sketch that never calls it still connects and registers, but its callbacks never run (a warning is logged once updates have waited 5 seconds). To keep the old behaviour, pass `SatelliteDispatchMode::IMMEDIATE`:

```cpp
// 0.0.1
void loop() {
    // ...
}

// 0.1.0
void loop() {
    satellite.loop();
    // ...
}

// or, to run callbacks on the network task as before
SatelliteOptions options;
options.dispatch = SatelliteDispatchMode::IMMEDIATE;
satellite.begin(options);
```

**Decode bitmaps inside the key state callback.** `state.bitmap` used to be a `String` holding base64. It is now a `SatelliteEncodedData` that points into the received message, valid only until the callback returns. Decode it there with `decode()`, which handles the base64 and the `data:` url of png/webp bitmaps:

```cpp
// 0.0.1
if (state.bitmap) {
    decodeAndDraw(state.bitmap.value()); // a base64 String
}

// 0.1.0
if (state.bitmap && !state.bitmap->isDataUrl()) {
    size_t length = state.bitmap->decode(buffer, sizeof(buffer)); // raw rgb bytes
}
```

**Subscriptions are objects.** `addSubscription()`, `removeSubscription()`, `subscriptionAction()` and `setOnSubscriptionState()` are replaced by `SatelliteSubscription`, which is re-registered after every reconnect and removed when destroyed. Its id is assigned for you (`getID()`):

```cpp
// 0.0.1
satellite.addSubscription("sub-1", "1/0/0", 72, String("rgb"), true, true, false);
satellite.setOnSubscriptionState([](const Satellite::SubscriptionState &state) { /* ... */ });
satellite.subscriptionAction("sub-1", SatelliteAction::PRESSED);

// 0.1.0
SatelliteSubscriptionConfig config = {.bitmapSize = 72, .bitmapFormat = "rgb"};
SatelliteSubscription subscription(satellite, "1/0/0", config);
subscription.setOnStateChange([](const SatelliteControlState &state) { /* ... */ });
subscription.action(SatelliteAction::PRESSED);
```

Everything else, in order of how likely it is to affect you:

| 0.0.1                                                                                   | 0.1.0                                                                                                                                          |
| --------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------- |
| `setOnLockedStateSet([](bool locked, uint8_t characters) {...})`                        | `setOnLockedStateSet([](const SatelliteLockedState &state) {...})`, with `locked`, `characterCount` and the new `rotation`                     |
| `esp_event` events on `SATELLITE_EVENTS` (`SATELLITE_EVENT_*`)                          | `satellite.setOnConnectionStateChange(...)` with a `SatelliteConnectionState`                                                                  |
| `isReady()` true once `BEGIN` is validated                                              | `isReady()` true once `CAPS` has also arrived, so `supportsSubscriptions()` and friends are accurate as soon as it is                          |
| Key presses and variables sent before Companion accepts a surface                       | Dropped until it does: check `surface.isRegistered()`, or use `setOnRegistered()` to send anything that needs to go out after each (re)connect |
| `MAX_RX_BUFFER_SIZE`, edited in the header                                              | `SatelliteOptions::maxLineLength`                                                                                                              |
| `DEFAULT_INSTANCE_NAME`, `DEFAULT_HOSTNAME`, `DEFAULT_PORT`                             | `Satellite::kDefaultDeviceName`, `Satellite::kDefaultHostname`, `Satellite::kDefaultApiPort`                                                   |
| `color`                                                                                 | `SatelliteColor` (`color` still works, but is deprecated)                                                                                      |
| `uint8_t` key indexes, `totalKeys` and `keysPerRow`                                     | `uint16_t`                                                                                                                                     |
| `parseSatelliteKeyType()`, `parseSatelliteColor()`, `SatelliteSurface::handleMessage()` | Removed (internal). The library parses messages, and `satellite_protocol::parseColor()` is there if you need it                                |

Callbacks declared with the old parameter types still compile: a key state lambda taking `const SatelliteSurfaceKeyState &` (now an alias of `SatelliteControlState`), and a variable lambda taking `String id, String value` by value.

## Examples

Arduino IDE sketches (`.ino`, no `platformio.ini`) - open directly in the Arduino IDE, or compile with `arduino-cli`:

- [`examples/basic`](examples/basic) - WiFi, a NeoPixel driven by key color, and a physical button reporting presses back to Companion.
- [`examples/encoder-led-ring`](examples/encoder-led-ring) - a layout manifest with a rotary encoder surrounded by a 24 LED NeoPixel ring (driven by Companion's LED ring styles) and a color-only button, sending multi-step rotations.
- [`examples/subscriptions`](examples/subscriptions) - follows and presses a single button by absolute location using `SatelliteSubscription`, instead of registering a full surface. **Button subscriptions must be enabled in Companion's settings** for this to work.
- [`examples/configuration-and-variables`](examples/configuration-and-variables) - a custom settings form (`CONFIG_FIELDS`/`DEVICE-CONFIG`, Companion -> device) alongside an input variable and an output variable (`setVariable()`/`setOnVariableSet()`, both directions).
- [`examples/pincode`](examples/pincode) - a surface that handles Companion's pincode-lock feature itself: shows the locked state and forwards digit presses with `pressPincodeKey()`.
- [`examples/cyd`](examples/cyd) - renders a 4x3 button surface directly on a "Cheap Yellow Display" (ESP32-2432S028R) touchscreen, decoding and drawing the actual bitmap Companion sends for each key, and reporting touches as button presses. Needs the extra `TFT_eSPI` and `XPT2046_Touchscreen` libraries, with TFT_eSPI set up from [`User_Setup_CYD.h`](examples/cyd/User_Setup_CYD.h) (see the top of `cyd.ino`); targets that one board rather than building across variants.

PlatformIO projects are prefixed `pio-` and have their own `platformio.ini`:

- [`examples/pio-exhaustive`](examples/pio-exhaustive) - runs through every public method and callback on `Satellite`, `SatelliteSurface` (grid and layout) and `SatelliteSubscription` once connected. No hardware beyond WiFi required, so it builds and runs the same on any variant.

All examples except `cyd` are compiled in CI across `esp32`, `esp32s2`, `esp32s3`, `esp32c3`, and `esp32c6` (see `.github/workflows/build-examples.yml`) - the `.ino` sketches via `arduino-cli`, `pio-exhaustive` via `pio run`. `cyd` is compiled for `esp32` only, since it targets one specific board.

## Tests

`test/host` builds on a desktop machine (`make -C test/host`, needs g++ and git):

- the protocol codec (`src/satellite-protocol.cpp`, which has no Arduino dependencies) is checked against Companion's own [conformance corpus](https://github.com/bitfocus/companion/tree/main/satellite-protocol/assets) for parsing, escaping and formatting
- `Satellite`, `SatelliteSurface` and `SatelliteSubscription` run against small stand-ins for the Arduino core and AsyncTCP, with the test playing Companion's side of the conversation (negotiation, registration, reconnects, keepalive timeouts, send buffering, coalescing, layouts...) under AddressSanitizer

## License

MIT, see [LICENSE](LICENSE).
