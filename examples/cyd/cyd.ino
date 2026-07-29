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
// have a different revision and will need to adjust the TFT_eSPI setup
// block and/or touch pins below.
//
// Library dependencies (install via Arduino Library Manager):
// - ESPAsyncWebServer (ESP32Async)
// - AsyncTCP (ESP32Async)
// - ArduinoJson
// - TFT_eSPI (Bodmer)
// - XPT2046_Touchscreen (Paul Stoffregen)

#include <Arduino.h>

// TFT_eSPI is normally configured by editing User_Setup.h inside the
// installed library folder. Defining USER_SETUP_LOADED and the settings it
// would otherwise read from that file - before including TFT_eSPI.h - lets
// this sketch configure the display itself instead, so nothing outside this
// file needs to be edited.
#define USER_SETUP_LOADED 1
#define ILI9341_DRIVER
#define TFT_WIDTH 240
#define TFT_HEIGHT 320
#define TFT_MISO 12
#define TFT_MOSI 13
#define TFT_SCLK 14
#define TFT_CS 15
#define TFT_DC 2
#define TFT_RST -1
#define TFT_BL 21
#define TFT_BACKLIGHT_ON HIGH
#define SPI_FREQUENCY 40000000
#define SPI_READ_FREQUENCY 20000000
#define SPI_TOUCH_FREQUENCY 2500000
#define USE_HSPI_PORT
#define LOAD_GLCD
#include <SPI.h>
#include <TFT_eSPI.h>

#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include "mbedtls/base64.h"
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
// base64-encoded - kCellWidth/kCellHeight are both 80px here, comfortably
// under the library's default MAX_RX_BUFFER_SIZE.
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

// Decode a base64 raw-RGB bitmap from Companion and blit it into a key's cell.
void drawCellBitmap(uint8_t index, const String &base64Data)
{
  size_t rawLen = 0;
  int sizeResult = mbedtls_base64_decode(nullptr, 0, &rawLen, (const unsigned char *)base64Data.c_str(), base64Data.length());
  if ((sizeResult != 0 && sizeResult != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL) || rawLen != (size_t)kBitmapSize * kBitmapSize * 3)
  {
    // Not the raw RGB size we expect (eg. a png/webp data URL) - skip it.
    return;
  }

  uint8_t *rgb888 = new uint8_t[rawLen];
  size_t decodedLen = 0;
  int decodeResult = mbedtls_base64_decode(rgb888, rawLen, &decodedLen, (const unsigned char *)base64Data.c_str(), base64Data.length());
  if (decodeResult != 0)
  {
    delete[] rgb888;
    return;
  }

  uint16_t *rgb565 = new uint16_t[kBitmapSize * kBitmapSize];
  for (uint16_t i = 0; i < kBitmapSize * kBitmapSize; i++)
  {
    rgb565[i] = tft.color565(rgb888[i * 3], rgb888[i * 3 + 1], rgb888[i * 3 + 2]);
  }
  delete[] rgb888;

  int16_t x, y, w, h;
  cellRect(index, x, y, w, h);
  tft.pushImage(x + (w - kBitmapSize) / 2, y + (h - kBitmapSize) / 2, kBitmapSize, kBitmapSize, rgb565);
  delete[] rgb565;

  tft.drawRect(x, y, w, h, TFT_DARKGREY);
}

// Map a raw touch reading to a key index, or -1 if outside the grid.
int8_t touchToKeyIndex(uint16_t rawX, uint16_t rawY)
{
  // The touch panel's axes are rotated relative to the display in landscape
  // orientation on this board, so X/Y are swapped here - see the
  // calibration note above if this doesn't match your unit.
  int16_t screenX = map(rawY, TOUCH_RAW_X_MIN, TOUCH_RAW_X_MAX, 0, kScreenWidth);
  int16_t screenY = map(rawX, TOUCH_RAW_Y_MIN, TOUCH_RAW_Y_MAX, 0, kScreenHeight);
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
                                  drawCellBitmap(state.index, state.bitmap.value());
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
