# cyd

Turns a "Cheap Yellow Display" into a 12-key Companion surface: a 4×3 grid of buttons drawn on its 2.8" screen with the images Companion renders, and touches sent back as presses. WiFi is set up from a phone, and the board's RGB LED shows its connection status.

## Ready-made firmware

Each [release](https://github.com/elliotmatson/companion-microsatellite/releases) has this example built for the ESP32-2432S028R, so you don't need the Arduino toolchain:

- `companion-cyd-<version>.bin` - the whole image (bootloader, partition table and app). Flash it at `0x0`, eg. `esptool.py --chip esp32 write_flash 0x0 companion-cyd-<version>.bin`.
- `companion-cyd-<version>-app.bin` - just the app, to update a board that already runs it: flash it at `0x10000`.

Then set it up as described below.

## Hardware

The common ESP32-2432S028R: an ESP32-WROOM-32, a 320×240 ILI9341 display and a resistive XPT2046 touch panel. Other CYD revisions (capacitive touch, USB-C, ESP32-S3 boards) are wired differently and need the pins changed in `User_Setup_CYD.h` and at the top of `cyd.ino`.

## Libraries

From the Arduino Library Manager:

- ESPAsyncWebServer and AsyncTCP (both by ESP32Async), and ArduinoJson
- TFT_eSPI (Bodmer)
- XPT2046_Touchscreen (Paul Stoffregen), 1.4 or later
- PNGdec (Larry Bank), 1.1 or later
- WiFiManager (tzapu), 2.0 or later

PlatformIO users: the PlatformIO registry's `paulstoffregen/XPT2046_Touchscreen` is an older snapshot without `begin(SPIClass &)`; use `https://github.com/PaulStoffregen/XPT2046_Touchscreen.git#v1.4`.

## Display setup (required)

TFT_eSPI reads its pins and driver from the `User_Setup.h` in its own library folder, and the library's code and the sketch must agree on them. Defining them in the sketch isn't enough: the library is compiled separately, never sees them, and the mismatch corrupts memory at runtime. So:

- **Arduino IDE / arduino-cli:** copy `User_Setup_CYD.h` over `User_Setup.h` in the TFT_eSPI library folder (eg. `~/Arduino/libraries/TFT_eSPI/`).
- **PlatformIO:** add `build_flags = -include path/to/User_Setup_CYD.h`.

If TFT_eSPI isn't set up for the CYD, the sketch stops the build with an `#error` pointing here, rather than building firmware that crashes. If colors look wrong on your panel, try `ILI9341_2_DRIVER` instead of `ILI9341_DRIVER` in `User_Setup_CYD.h`.

## Setup

1. Flash it. With no WiFi network saved, it starts a WiFi network of its own called `CYD-Companion-XXXX` and shows its name on the screen.
2. Join that network from a phone. The setup page should open by itself; if not, browse to `http://192.168.4.1`.
3. Pick your WiFi network and enter its password. Optionally, enter Companion's address (`192.168.1.10`, or `host:port` if it isn't on 16622) - or leave it empty and add the device from Companion's Surfaces page, where it's discovered automatically.
4. "CYD Surface" appears in Companion with a 4×3 grid. Buttons on the screen show what Companion draws, and tapping one presses it.

If the saved network can't be reached when it starts (eg. after a power cut, before the router is back), it opens the setup network for 5 minutes, then restarts and tries again.

Until Companion has the surface, the screen says what it's waiting for and shows the board's IP address.

## BOOT button

- **Short press:** touch calibration (below).
- **Hold for 3 seconds:** forget the WiFi network and restart into setup. The screen counts down from when you've held it for a second; let go before the end to cancel.

## Status LED

| LED                        | Meaning                                                    |
| -------------------------- | ---------------------------------------------------------- |
| Blue, blinking             | WiFi setup: join the `CYD-Companion-XXXX` network          |
| Blue                       | Joining the WiFi network                                   |
| Cyan, blinking             | On WiFi, looking for Companion                             |
| Green                      | Connected, and the surface is registered with Companion    |
| Red and white, alternating | Companion is too old for this library (needs 4.3 or later) |
| White, blinking fast       | BOOT is held: let go to cancel forgetting WiFi             |

The states don't depend on red, as on some boards the red part of the LED isn't connected.

## Touch calibration

Resistive panels vary. If taps land on the wrong key, calibrate: press the BOOT button, then tap the cross near the top-left corner and the one near the bottom-right. The result is saved in flash and survives reflashing; the `TOUCH_RAW_*` defines are only defaults.

## Performance

- **PNG bitmaps:** the surface asks Companion for PNG (`bitmapFormat = "png"`), about 18KB for a page of 12 keys against ~307KB as raw RGB. Companion versions whose Satellite API is older than 1.12 can't send PNG; their raw RGB is drawn too.
- **DMA drawing:** PNG rows are decoded into 16-row bands, and each band goes to the display by DMA while the next is decoded. A key takes about 7ms to draw.
- **Receive buffers:** `maxLineLength` is sized to one raw RGB key and `maxQueuedBytes` to 96KB, so a couple of keys can queue while the next arrives, and receiving and drawing overlap.

The PNG decoder takes ~44KB of RAM, kept for good. Free heap with everything running is about 150KB.
