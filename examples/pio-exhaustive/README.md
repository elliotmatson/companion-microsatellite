# pio-exhaustive

A PlatformIO project that exercises every public method and callback in the library once it's connected, as a reference for what's available and a quick check on real hardware. It isn't interactive: watch the serial log.

## What it sets up

- An 8-key grid surface: 72px PNG bitmaps, colors, styled text, brightness, full pincode support, a config field, page changes (once allowed in Companion's surface settings), and one input and one output variable.
- A layout surface with four controls: a 64×64 bitmap key, two color-only LED buttons, and an encoder with a 24-LED ring.
- A button subscription at `1/0/0`, also with 72px PNG bitmaps.
- Every callback, each logging what it receives.
- `SatelliteDispatchMode::LOOP` and a 32KB `maxLineLength`.

When the connection is ready and both surfaces have registered, it runs through the API once: it prints the negotiated feature table and bitmap formats, then sends presses, rotations (with and without rotation amounts), a variable, pincode digits, firmware update info, page changes, layout control actions, and subscription actions if subscriptions are enabled. It runs again after every reconnect.

## Building

It needs no hardware beyond WiFi, and builds for `esp32`, `esp32s2`, `esp32s3`, `esp32c3` and `esp32c6`. Use PlatformIO 6.1.x: 6.2.0 bundles a newer SCons that the pinned pioarduino platform doesn't work with.

```sh
pip install "platformio==6.1.19"
pio run -d examples/pio-exhaustive -e esp32 -t upload
pio device monitor -d examples/pio-exhaustive
```

It builds against the library in this repository (`lib_deps = symlink://../..`), so it always tests your working copy. The app is over 1MB, so it uses the 1.5MB single-app partition table.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` at the top of `src/main.cpp`.
2. Point it at Companion. This example serves its REST API on port **9998** rather than the default 9999, to exercise that constructor:

   ```sh
   curl -X POST -H 'Content-Type: application/json' -d '{"host":"<companion-ip>","port":16622}' http://<device-ip>:9998/api/config
   ```

3. For the subscription part, enable button subscriptions in Companion's settings.

Library logs use the `Satellite`, `SatelliteSurface` and `SatelliteSubscription` tags at INFO level and above. To see their DEBUG logs too, raise `CONFIG_LOG_MAXIMUM_LEVEL` in the sdkconfig and call `esp_log_level_set()` for those tags.
