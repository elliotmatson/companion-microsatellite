# cyd

Turns a "Cheap Yellow Display" into a 12-key Companion surface: a 4×3 grid of buttons drawn on its 2.8" screen with the images Companion renders, and touches sent back as presses.

## Hardware

The common ESP32-2432S028R: an ESP32-WROOM-32, a 320×240 ILI9341 display and a resistive XPT2046 touch panel. Other CYD revisions (capacitive touch, USB-C, ESP32-S3 boards) are wired differently and need the pins changed in `User_Setup_CYD.h` and at the top of `cyd.ino`.

## Libraries

From the Arduino Library Manager:

- ESPAsyncWebServer and AsyncTCP (both by ESP32Async), and ArduinoJson
- TFT_eSPI (Bodmer)
- XPT2046_Touchscreen (Paul Stoffregen), 1.4 or later
- PNGdec (Larry Bank), 1.1 or later

PlatformIO users: the PlatformIO registry's `paulstoffregen/XPT2046_Touchscreen` is an older snapshot without `begin(SPIClass &)`; use `https://github.com/PaulStoffregen/XPT2046_Touchscreen.git#v1.4`.

## Display setup (required)

TFT_eSPI reads its pins and driver from the `User_Setup.h` in its own library folder, and the library's code and the sketch must agree on them. Defining them in the sketch isn't enough: the library is compiled separately, never sees them, and the mismatch corrupts memory at runtime. So:

- **Arduino IDE / arduino-cli:** copy `User_Setup_CYD.h` over `User_Setup.h` in the TFT_eSPI library folder (eg. `~/Arduino/libraries/TFT_eSPI/`).
- **PlatformIO:** add `build_flags = -include path/to/User_Setup_CYD.h`.

If TFT_eSPI isn't set up for the CYD, the sketch stops the build with an `#error` pointing here, rather than building firmware that crashes. If colors look wrong on your panel, try `ILI9341_2_DRIVER` instead of `ILI9341_DRIVER` in `User_Setup_CYD.h`.

## Setup

1. Set `WIFI_SSID` and `WIFI_PASSWORD` near the top of `cyd.ino`.
2. Flash it and point it at Companion: let Companion discover it, or set the target over the REST API on port 9999 (see [Usage](../../README.md#usage)).
3. "CYD Surface" appears in Companion with a 4×3 grid. Buttons on the screen show what Companion draws, and tapping one presses it.

## Touch calibration

Resistive panels vary. If taps land on the wrong key, calibrate: hold a finger on the screen while the board starts up, or press the BOOT button once it's running, then tap the cross near the top-left corner and the one near the bottom-right. The result is saved in flash and survives reflashing; the `TOUCH_RAW_*` defines are only defaults.

## Performance

- **PNG bitmaps:** the surface asks Companion for PNG (`bitmapFormat = "png"`), about 18KB for a page of 12 keys against ~307KB as raw RGB. Companion versions whose Satellite API is older than 1.12 can't send PNG; their raw RGB is drawn too.
- **DMA drawing:** PNG rows are decoded into 16-row bands, and each band goes to the display by DMA while the next is decoded. A key takes about 7ms to draw.
- **Receive buffers:** `maxLineLength` is sized to one raw RGB key and `maxQueuedBytes` to 96KB, so a couple of keys can queue while the next arrives, and receiving and drawing overlap.

The PNG decoder takes ~44KB of RAM, kept for good. Free heap with everything running is about 150KB.
