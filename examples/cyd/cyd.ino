// Companion Microsatellite - "Cheap Yellow Display" touchscreen surface.
//
// Renders a 4x3 (4 columns, 3 rows = 12 keys) Companion surface directly on
// the board's built-in 2.8" TFT, showing the actual button image Companion
// sends for each key, and reports touches back as button presses - no
// external buttons or NeoPixels needed.
//
// Targets the classic ESP32-WROOM-32 "Cheap Yellow Display" board sold as
// ESP32-2432S028R: a 320x240 ILI9341 SPI TFT with a resistive XPT2046
// touch controller. There are several CYD hardware revisions (capacitive
// touch, USB-C, ESP32-S3 variants, etc.) with different pin wiring - the
// pin numbers below match the common resistive-touch ESP32-2432S028R.
// If your display is garbled or touch doesn't line up, you very likely
// have a different revision and will need to adjust User_Setup_CYD.h
// and/or the touch pins below.
//
// Library dependencies (install via Arduino Library Manager):
// - ESPAsyncWebServer (ESP32Async)
// - AsyncTCP (ESP32Async)
// - ArduinoJson
// - TFT_eSPI (Bodmer)
// - XPT2046_Touchscreen (Paul Stoffregen), 1.4 or later
//
// TFT_eSPI setup: TFT_eSPI takes its display settings from the User_Setup.h
// in its own library folder, and those settings have to be the same for the
// library's code and this sketch. Defining them in the sketch instead isn't
// enough: the library is compiled separately and never sees them, and the
// mismatch corrupts memory at runtime. So use the User_Setup_CYD.h next to
// this sketch:
// - Arduino IDE / arduino-cli: copy User_Setup_CYD.h over User_Setup.h in
//   the TFT_eSPI library folder (eg. ~/Arduino/libraries/TFT_eSPI/).
// - PlatformIO: add it to every file with
//     build_flags = -include path/to/User_Setup_CYD.h

#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>

// Fail the build, rather than the board, if TFT_eSPI isn't set up for the CYD
// (see above). Its default setup is for an ESP8266 NodeMCU.
#if !(defined(ILI9341_DRIVER) || defined(ILI9341_2_DRIVER)) || TFT_MOSI != 13 || TFT_SCLK != 14 || TFT_CS != 15 || TFT_DC != 2
#error "TFT_eSPI isn't configured for the CYD - install User_Setup_CYD.h as described at the top of cyd.ino"
#endif

#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include "companion-satellite.hpp"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Resistive touch controller pins (separate SPI bus from the display).
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// Raw touch calibration - these are reasonable defaults for this board, but
// resistive touch panels vary unit to unit. If touches feel offset, adjust
// these to match your panel (print touchscreen.getPoint() to calibrate).
#define TOUCH_RAW_X_MIN 200
#define TOUCH_RAW_X_MAX 3700
#define TOUCH_RAW_Y_MIN 240
#define TOUCH_RAW_Y_MAX 3800

// Display dimensions in landscape orientation.
constexpr int16_t kScreenWidth = 320;
constexpr int16_t kScreenHeight = 240;

// Surface grid: 4 columns x 3 rows = 12 keys, matching KEYS_PER_ROW/KEYS_TOTAL below.
constexpr uint8_t kGridColumns = 4;
constexpr uint8_t kGridRows = 3;
constexpr uint8_t kKeyCount = kGridColumns * kGridRows;
constexpr int16_t kCellWidth = kScreenWidth / kGridColumns;
constexpr int16_t kCellHeight = kScreenHeight / kGridRows;

// Requested square bitmap size, matching the cell size exactly so each
// button image fills its cell. Sent as raw 8-bit RGB, 3 bytes per pixel,
// base64-encoded - kCellWidth/kCellHeight are both 80px here, so each
// KEY-STATE line is ~26KB, under the default SatelliteOptions::maxLineLength
// (32KB).
constexpr uint16_t kBitmapSize = 80;

TFT_eSPI tft = TFT_eSPI();
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

Satellite satellite("microsatellite-cyd");

SatelliteSurfaceConfig surfaceConfig = {
    .totalKeys = kKeyCount,
    .keysPerRow = kGridColumns,
    .sendColors = true,
    .bitmapSize = kBitmapSize,
};

SatelliteSurface surface(satellite, "CYD Surface", surfaceConfig);

// Index of the key currently held down by touch, or -1 if none.
int8_t activeKeyIndex = -1;

// Compute the on-screen rectangle for a key index (0 = top-left, increasing left-to-right then top-to-bottom).
void cellRect(uint8_t index, int16_t &x, int16_t &y, int16_t &w, int16_t &h)
{
  uint8_t col = index % kGridColumns;
  uint8_t row = index / kGridColumns;
  x = col * kCellWidth;
  y = row * kCellHeight;
  w = kCellWidth;
  h = kCellHeight;
}

// Fill a key's cell with a flat color, used before the first bitmap arrives and as a fallback.
void drawCellColor(uint8_t index, uint16_t fillColor)
{
  int16_t x, y, w, h;
  cellRect(index, x, y, w, h);
  tft.fillRect(x + 1, y + 1, w - 2, h - 2, fillColor);
  tft.drawRect(x, y, w, h, TFT_DARKGREY);
}

// Decode a raw-RGB bitmap from Companion and blit it into a key's cell.
// The bitmap data is only valid during the key state callback, so this is
// called from inside it (which runs in satellite.loop(), on the loop task -
// the same task that reads the touchscreen, so the two never share SPI
// concurrently).
void drawCellBitmap(uint8_t index, const SatelliteEncodedData &bitmap)
{
  constexpr size_t kRawLength = (size_t)kBitmapSize * kBitmapSize * 3;
  if (bitmap.isDataUrl() || bitmap.decodedLength() != kRawLength)
  {
    // Not the raw RGB size we expect (eg. a png/webp data URL) - skip it.
    return;
  }

  // Decode and convert in one buffer: each 3-byte RGB888 pixel shrinks to a
  // 2-byte RGB565 one, so converting front to back never overwrites input
  // that hasn't been read yet. One allocation instead of two.
  constexpr size_t kPixels = (size_t)kBitmapSize * kBitmapSize;
  uint16_t *rgb565 = new (std::nothrow) uint16_t[(kRawLength + 1) / 2];
  if (!rgb565)
  {
    return;
  }
  uint8_t *rgb888 = reinterpret_cast<uint8_t *>(rgb565);
  if (bitmap.decode(rgb888, kRawLength) != kRawLength)
  {
    delete[] rgb565;
    return;
  }

  for (size_t i = 0; i < kPixels; i++)
  {
    uint8_t r = rgb888[i * 3], g = rgb888[i * 3 + 1], b = rgb888[i * 3 + 2];
    rgb565[i] = tft.color565(r, g, b);
  }

  int16_t x, y, w, h;
  cellRect(index, x, y, w, h);
  tft.pushImage(x + (w - kBitmapSize) / 2, y + (h - kBitmapSize) / 2, kBitmapSize, kBitmapSize, rgb565);
  delete[] rgb565;

  tft.drawRect(x, y, w, h, TFT_DARKGREY);
}

// Map a raw touch reading to a key index, or -1 if outside the grid.
int8_t touchToKeyIndex(uint16_t rawX, uint16_t rawY)
{
  // touchscreen.setRotation(1) in setup() already turns the raw readings to
  // match the landscape display, so x follows the screen's x and y its y -
  // see the calibration note above if touches feel offset on your unit.
  int16_t screenX = map(rawX, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, kScreenWidth);
  int16_t screenY = map(rawY, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, kScreenHeight);
  screenX = constrain(screenX, 0, kScreenWidth - 1);
  screenY = constrain(screenY, 0, kScreenHeight - 1);

  uint8_t col = screenX / kCellWidth;
  uint8_t row = screenY / kCellHeight;
  uint8_t index = row * kGridColumns + col;
  return (index < kKeyCount) ? static_cast<int8_t>(index) : -1;
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  tft.init();
  tft.setRotation(1); // landscape; try 3 if the display looks upside down
  // drawCellBitmap() builds its RGB565 pixels as native (little-endian)
  // uint16_t values; pushImage() sends them high byte first unless told to swap.
  tft.setSwapBytes(true);
  tft.fillScreen(TFT_BLACK);

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(1);

  for (uint8_t i = 0; i < kKeyCount; i++)
  {
    drawCellColor(i, TFT_BLACK);
  }

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  satellite.begin();

  // Companion -> device: draw the bitmap it sent for this key, or fall back
  // to a flat color fill if no bitmap is available yet.
  surface.setOnKeyStateChange([](const SatelliteSurfaceKeyState &state)
                              {
                                if (state.index >= kKeyCount)
                                {
                                  return;
                                }

                                if (state.bitmap.has_value())
                                {
                                  drawCellBitmap(state.index, *state.bitmap);
                                }
                                else
                                {
                                  uint16_t fillColor = state.keyColor.has_value()
                                                            ? tft.color565(state.keyColor->r, state.keyColor->g, state.keyColor->b)
                                                            : TFT_BLACK;
                                  drawCellColor(state.index, fillColor);
                                } });

  surface.setOnKeysClear([]()
                         {
                           for (uint8_t i = 0; i < kKeyCount; i++)
                           {
                             drawCellColor(i, TFT_BLACK);
                           } });
}

void loop()
{
  // Draws any key updates received since last time (see drawCellBitmap).
  satellite.loop();

  if (touchscreen.touched())
  {
    TS_Point point = touchscreen.getPoint();
    int8_t index = touchToKeyIndex(point.x, point.y);

    if (index != activeKeyIndex)
    {
      // Release whichever key was previously held, then press the new one.
      if (activeKeyIndex != -1)
      {
        surface.buttonAction(activeKeyIndex, SatelliteAction::RELEASED);
      }
      if (index != -1)
      {
        surface.buttonAction(index, SatelliteAction::PRESSED);
      }
      activeKeyIndex = index;
    }
  }
  else if (activeKeyIndex != -1)
  {
    surface.buttonAction(activeKeyIndex, SatelliteAction::RELEASED);
    activeKeyIndex = -1;
  }

  delay(20);
}
