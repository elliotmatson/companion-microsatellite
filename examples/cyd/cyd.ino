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

#include <Preferences.h>
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

// The BOOT button, read once the sketch is running (holding it during reset
// puts the ESP32 into download mode instead).
#define BOOT_BUTTON 0

// Raw touch calibration: the raw readings at the screen's left/right and
// top/bottom edges. These defaults suit most of these boards, but resistive
// touch panels vary unit to unit - if touches feel offset, hold a finger on
// the screen while the board starts up, or press the BOOT button once it's
// running, to run the on-screen calibration. It saves its result in flash
// and is used from then on.
#define TOUCH_RAW_X_MIN 200
#define TOUCH_RAW_X_MAX 3700
#define TOUCH_RAW_Y_MIN 240
#define TOUCH_RAW_Y_MAX 3800

struct TouchCalibration
{
  int16_t xMin = TOUCH_RAW_X_MIN;
  int16_t xMax = TOUCH_RAW_X_MAX;
  int16_t yMin = TOUCH_RAW_Y_MIN;
  int16_t yMax = TOUCH_RAW_Y_MAX;
};
TouchCalibration touchCalibration;

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
// KEY-STATE line is ~26KB.
constexpr uint16_t kBitmapSize = 80;

// The longest KEY-STATE line to expect: the base64 bitmap (4 characters per
// 3 bytes) plus 1KB for the rest of the message.
constexpr size_t kMaxLineLength = ((size_t)kBitmapSize * kBitmapSize * 3 + 2) / 3 * 4 + 1024;

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
  // that hasn't been read yet. It's static rather than allocated per key: no
  // heap churn while a page of images streams in, and nothing to run out of
  // (on builds without C++ exceptions, a failed new aborts even with nothrow).
  constexpr size_t kPixels = (size_t)kBitmapSize * kBitmapSize;
  static uint16_t rgb565[(kRawLength + 1) / 2];
  uint8_t *rgb888 = reinterpret_cast<uint8_t *>(rgb565);
  if (bitmap.decode(rgb888, kRawLength) != kRawLength)
  {
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

  tft.drawRect(x, y, w, h, TFT_DARKGREY);
}

// Load a touch calibration saved by runTouchCalibration(), if there is one.
void loadTouchCalibration()
{
  Preferences prefs;
  prefs.begin("cyd-touch");
  if (prefs.isKey("xMin"))
  {
    touchCalibration.xMin = prefs.getShort("xMin");
    touchCalibration.xMax = prefs.getShort("xMax");
    touchCalibration.yMin = prefs.getShort("yMin");
    touchCalibration.yMax = prefs.getShort("yMax");
  }
  prefs.end();
}

// Wait for a tap, and return the average raw reading while the finger was down.
TS_Point readCalibrationTap()
{
  for (;;)
  {
    while (touchscreen.touched())
    {
      delay(10); // let go of any earlier touch first
    }
    while (!touchscreen.touched())
    {
      delay(10);
    }

    int32_t sumX = 0, sumY = 0;
    uint16_t samples = 0;
    while (touchscreen.touched())
    {
      TS_Point point = touchscreen.getPoint();
      sumX += point.x;
      sumY += point.y;
      samples++;
      delay(10);
    }
    if (samples >= 5) // ignore brushes and bounces
    {
      return TS_Point(sumX / samples, sumY / samples, 0);
    }
  }
}

void drawCrosshair(int16_t x, int16_t y, uint16_t color)
{
  tft.drawFastHLine(x - 10, y, 21, color);
  tft.drawFastVLine(x, y - 10, 21, color);
}

// On-screen calibration: tap a cross near the top-left corner, then one near
// the bottom-right, and the raw readings at the screen edges are worked out
// from those two and saved to flash.
void runTouchCalibration()
{
  constexpr int16_t kInset = 20;
  const int16_t x0 = kInset, y0 = kInset;
  const int16_t x1 = kScreenWidth - 1 - kInset, y1 = kScreenHeight - 1 - kInset;

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.drawString("Tap each cross", kScreenWidth / 2, kScreenHeight / 2);

  drawCrosshair(x0, y0, TFT_WHITE);
  TS_Point topLeft = readCalibrationTap();
  drawCrosshair(x0, y0, TFT_BLACK);
  drawCrosshair(x1, y1, TFT_WHITE);
  TS_Point bottomRight = readCalibrationTap();
  drawCrosshair(x1, y1, TFT_BLACK);

  // A small spread means the same spot was tapped twice (or the panel's
  // axes don't match the display) - keep the old calibration then.
  if (abs(bottomRight.x - topLeft.x) < 1000 || abs(bottomRight.y - topLeft.y) < 1000)
  {
    Serial.println("Touch calibration: taps too close together, keeping the previous calibration");
    tft.drawString("Didn't work - try again", kScreenWidth / 2, kScreenHeight / 2);
  }
  else
  {
    float xPerPixel = float(bottomRight.x - topLeft.x) / (x1 - x0);
    float yPerPixel = float(bottomRight.y - topLeft.y) / (y1 - y0);
    touchCalibration.xMin = topLeft.x - xPerPixel * x0;
    touchCalibration.xMax = topLeft.x + xPerPixel * (kScreenWidth - x0);
    touchCalibration.yMin = topLeft.y - yPerPixel * y0;
    touchCalibration.yMax = topLeft.y + yPerPixel * (kScreenHeight - y0);

    Preferences prefs;
    prefs.begin("cyd-touch");
    prefs.putShort("xMin", touchCalibration.xMin);
    prefs.putShort("xMax", touchCalibration.xMax);
    prefs.putShort("yMin", touchCalibration.yMin);
    prefs.putShort("yMax", touchCalibration.yMax);
    prefs.end();

    Serial.printf("Touch calibration saved: x %d..%d, y %d..%d\n", touchCalibration.xMin, touchCalibration.xMax,
                  touchCalibration.yMin, touchCalibration.yMax);
    tft.drawString("Calibration saved", kScreenWidth / 2, kScreenHeight / 2);
  }
  delay(1500);
  tft.setTextSize(1);
  tft.fillScreen(TFT_BLACK);
}

// Map a raw touch reading to a key index, or -1 if outside the grid.
int8_t touchToKeyIndex(uint16_t rawX, uint16_t rawY)
{
  // touchscreen.setRotation(1) in setup() already turns the raw readings to
  // match the landscape display, so x follows the screen's x and y its y -
  // see the calibration note above if touches feel offset on your unit.
  int16_t screenX = map(rawX, touchCalibration.xMin, touchCalibration.xMax, 0, kScreenWidth);
  int16_t screenY = map(rawY, touchCalibration.yMin, touchCalibration.yMax, 0, kScreenHeight);
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

  // Holding a finger on the screen during startup runs the touch calibration.
  loadTouchCalibration();
  if (touchscreen.touched())
  {
    runTouchCalibration();
  }
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

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

  // Size the receive buffers for this sketch's bitmaps: a line just long enough for one
  // key, and room to queue a couple while the next arrives, so receiving and drawing
  // overlap. (With the defaults, receiving pauses after every key until it's drawn.)
  SatelliteOptions options;
  options.maxLineLength = kMaxLineLength;
  options.maxQueuedBytes = 96 * 1024;
  satellite.begin(options);

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

  // The BOOT button runs the touch calibration too.
  if (digitalRead(BOOT_BUTTON) == LOW)
  {
    runTouchCalibration();
    for (uint8_t i = 0; i < kKeyCount; i++)
    {
      drawCellColor(i, TFT_BLACK);
    }
    // The calibration screen wiped the keys: reconnecting makes Companion send them all again.
    satellite.setTarget(satellite.getTargetHostname(), satellite.getTargetPort());
    activeKeyIndex = -1;
  }

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
