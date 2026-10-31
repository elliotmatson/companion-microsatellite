// Companion Microsatellite - "Cheap Yellow Display" touchscreen surface.
//
// Renders a 4x3 (4 columns, 3 rows = 12 keys) Companion surface directly on
// the board's built-in 2.8" TFT, showing the actual button image Companion
// sends for each key, and reports touches back as button presses - no
// external buttons or NeoPixels needed.
//
// WiFi is set up from a phone: with no saved network (or if it can't be
// reached) the board starts its own access point with a setup page, where
// you pick the network and can enter Companion's address. The board's RGB
// LED shows where it's got to, and the BOOT button calibrates the touch
// screen (short press) or forgets the WiFi network (hold for 3 seconds).
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
// - PNGdec (Larry Bank), 1.1 or later
// - WiFiManager (tzapu), 2.0 or later
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

#include <PNGdec.h>
#include <Preferences.h>
#include <XPT2046_Touchscreen.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "companion-satellite.hpp"

// How long the WiFi setup page waits for someone before the board restarts
// and tries the saved network again (eg. after a power cut, when the board
// came up before the router).
constexpr uint32_t kWifiSetupTimeoutSeconds = 300;

// Resistive touch controller pins (separate SPI bus from the display).
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

// The BOOT button, read once the sketch is running (holding it during reset
// puts the ESP32 into download mode instead). A short press runs the touch
// calibration; holding it forgets the WiFi network and restarts into setup.
#define BOOT_BUTTON 0
constexpr uint32_t kBootShortPressMs = 1000;
constexpr uint32_t kBootWifiResetMs = 3000;

// The RGB status LED. Its pins are pulled low to light it.
#define LED_RED 4
#define LED_GREEN 16
#define LED_BLUE 17
constexpr uint8_t kLedBrightness = 24; // out of 255 - full brightness is glaring

// Raw touch calibration: the raw readings at the screen's left/right and
// top/bottom edges. These defaults suit most of these boards, but resistive
// touch panels vary unit to unit - if touches feel offset, press the BOOT
// button to run the on-screen calibration. It saves its result in flash and
// is used from then on.
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
// button image fills its cell (kCellWidth/kCellHeight are both 80px here).
// Bitmaps are requested as PNG: a typical button is 1-6KB instead of the
// 19.2KB of raw 8-bit RGB, so a page of 12 is several times less to send.
// A Companion whose Satellite API is older than 1.12 can't send PNG and sends
// raw RGB, which is drawn too.
constexpr uint16_t kBitmapSize = 80;
constexpr size_t kRawBitmapLength = (size_t)kBitmapSize * kBitmapSize * 3;

// The longest KEY-STATE line to expect: a raw RGB bitmap in base64 (4
// characters per 3 bytes) plus 1KB for the rest of the message. PNG lines are
// smaller.
constexpr size_t kMaxLineLength = (kRawBitmapLength + 2) / 3 * 4 + 1024;

TFT_eSPI tft = TFT_eSPI();
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

Satellite satellite("microsatellite-cyd");

SatelliteSurfaceConfig surfaceConfig = {
    .totalKeys = kKeyCount,
    .keysPerRow = kGridColumns,
    .sendColors = true,
    .bitmapSize = kBitmapSize,
    .bitmapFormat = "png",
};

SatelliteSurface surface(satellite, "CYD Surface", surfaceConfig);

// Index of the key currently held down by touch, or -1 if none.
int8_t activeKeyIndex = -1;

// Set while something else is on the screen (the BOOT countdown), so key
// updates from Companion don't draw over it. They're all sent again after.
bool keysHidden = false;

// WiFi setup: a captive portal on the board's own access point. Besides the
// network, it asks (optionally) for Companion's address, as "host" or
// "host:port" - leave it empty to keep the current one, or to let Companion
// find this device itself (Surfaces > Discover).
WiFiManager wifiManager;
WiFiManagerParameter companionParam("companion", "Companion IP or hostname (optional)", "", 64);
String companionFromSetup; // what was entered, applied once the satellite starts

// What the status LED shows. Each state can be told apart with just green
// and blue, as on some boards the red part of the LED isn't connected. (Up
// here, ahead of every function, so the prototypes the Arduino IDE
// generates for them can use it.)
enum class LedStatus
{
  WIFI_SETUP,           // blue, blinking: setup access point is up
  WIFI_CONNECTING,      // blue: joining the WiFi network
  COMPANION_CONNECTING, // cyan, blinking: on WiFi, looking for Companion
  READY,                // green: connected, and Companion has the surface
  REJECTED,             // red and white, alternating: Companion is too old for this library
  WIFI_RESET_HOLD,      // white, blinking fast: BOOT held, about to forget WiFi
};

// Decoded PNG rows are collected into bands, and each band goes to the
// display by DMA while PNGdec decompresses the next into the other buffer -
// the display transfer then mostly hides behind the decompression, instead of
// the CPU waiting on 80 separate row writes.
constexpr int kPngBandRows = 16;
uint16_t pngBands[2][kPngBandRows * kBitmapSize];
bool dmaReady = false; // tft.initDMA() worked (otherwise bands are pushed the ordinary way)

// The PNG being drawn, for pngDrawRow(). (Up here with the other globals, ahead of
// every function, so the prototypes the Arduino IDE generates for them can use it.)
struct PngTarget
{
  int16_t x;
  int16_t y;
  int width;
  int band;        // which of pngBands is being filled
  int bandRows;    // rows in it so far
  int bandStartY;  // image row its first row came from
};

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

// One buffer for decoding bitmaps, static rather than allocated per key: no
// heap churn while a page of images streams in, and nothing to run out of
// (on builds without C++ exceptions, a failed new aborts even with nothrow).
// It holds a raw RGB bitmap, or a PNG file - which for a button image is far
// smaller than the raw pixels.
uint16_t bitmapBuffer[(kRawBitmapLength + 1) / 2];

// The PNG decoder. It's ~44KB (mostly zlib's 32KB window), so there's just
// the one, kept for good rather than allocated per image.
PNG png;

// Send the band being filled to the display, and switch to the other one.
void flushPngBand(PngTarget *target)
{
  if (target->bandRows == 0)
  {
    return;
  }
  uint16_t *band = pngBands[target->band];
  int16_t y = target->y + target->bandStartY;
  if (dmaReady)
  {
    // Waits for the previous band's transfer (from the other buffer) to finish, then
    // starts this one and returns, so the next band can be decoded meanwhile
    tft.pushImageDMA(target->x, y, target->width, target->bandRows, band);
  }
  else
  {
    tft.pushImage(target->x, y, target->width, target->bandRows, band);
  }
  target->band ^= 1;
  target->bandRows = 0;
}

// PNGdec hands over the image a row at a time: convert it into the current band.
int pngDrawRow(PNGDRAW *draw)
{
  PngTarget *target = static_cast<PngTarget *>(draw->pUser);
  if (target->bandRows == 0)
  {
    target->bandStartY = draw->y;
  }
  target->width = draw->iWidth;
  uint16_t *row = pngBands[target->band] + target->bandRows * draw->iWidth;
  // Big-endian is the display's own byte order, so the band can go out as it is
  png.getLineAsRGB565(draw, row, PNG_RGB565_BIG_ENDIAN, 0xffffffff);
  if (++target->bandRows == kPngBandRows)
  {
    flushPngBand(target);
  }
  return 1;
}

// Decode a PNG bitmap from Companion straight onto the screen.
bool drawPngBitmap(int16_t x, int16_t y, const SatelliteEncodedData &bitmap)
{
  if (bitmap.mimeType() != "image/png")
  {
    return false;
  }
  uint8_t *file = reinterpret_cast<uint8_t *>(bitmapBuffer);
  size_t length = bitmap.decode(file, sizeof(bitmapBuffer));
  if (length == 0 || png.openRAM(file, length, pngDrawRow) != PNG_SUCCESS)
  {
    return false;
  }
  bool fits = png.getWidth() <= kBitmapSize && png.getHeight() <= kBitmapSize;
  if (fits)
  {
    PngTarget target{x, y, 0, 0, 0, 0};
    // The bands are already in the display's byte order, and pushImageDMA() would
    // otherwise swap them in place
    bool swapBytes = tft.getSwapBytes();
    tft.setSwapBytes(false);
    tft.startWrite(); // one SPI transaction for the whole image
    png.decode(&target, 0);
    flushPngBand(&target); // the last, partial band
    tft.dmaWait();
    tft.endWrite();
    tft.setSwapBytes(swapBytes);
  }
  png.close();
  return fits;
}

// Decode a raw RGB bitmap from Companion and draw it.
bool drawRawBitmap(int16_t x, int16_t y, const SatelliteEncodedData &bitmap)
{
  if (bitmap.isDataUrl() || bitmap.decodedLength() != kRawBitmapLength)
  {
    return false;
  }

  // Decode and convert in one buffer: each 3-byte RGB888 pixel shrinks to a
  // 2-byte RGB565 one, so converting front to back never overwrites input
  // that hasn't been read yet.
  constexpr size_t kPixels = (size_t)kBitmapSize * kBitmapSize;
  uint8_t *rgb888 = reinterpret_cast<uint8_t *>(bitmapBuffer);
  if (bitmap.decode(rgb888, kRawBitmapLength) != kRawBitmapLength)
  {
    return false;
  }
  for (size_t i = 0; i < kPixels; i++)
  {
    uint8_t r = rgb888[i * 3], g = rgb888[i * 3 + 1], b = rgb888[i * 3 + 2];
    bitmapBuffer[i] = tft.color565(r, g, b);
  }
  tft.pushImage(x, y, kBitmapSize, kBitmapSize, bitmapBuffer);
  return true;
}

// Draw the bitmap Companion sent for a key into its cell. The bitmap data is
// only valid during the key state callback, so this is called from inside it
// (which runs in satellite.loop(), on the loop task - the same task that reads
// the touchscreen, so the two never share SPI concurrently).
void drawCellBitmap(uint8_t index, const SatelliteEncodedData &bitmap)
{
  int16_t x, y, w, h;
  cellRect(index, x, y, w, h);
  int16_t imageX = x + (w - kBitmapSize) / 2, imageY = y + (h - kBitmapSize) / 2;
  if (!drawPngBitmap(imageX, imageY, bitmap) && !drawRawBitmap(imageX, imageY, bitmap))
  {
    return; // not something we can draw (eg. the wrong size) - leave the cell as it was
  }
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

// Light the RGB LED, each channel 0 (off) to 1 (kLedBrightness).
void setLed(bool red, bool green, bool blue)
{
  // The LED is lit by pulling its pins low, so full duty cycle is off
  analogWrite(LED_RED, red ? 255 - kLedBrightness : 255);
  analogWrite(LED_GREEN, green ? 255 - kLedBrightness : 255);
  analogWrite(LED_BLUE, blue ? 255 - kLedBrightness : 255);
}

void showLedStatus(LedStatus status)
{
  bool blinkSlow = (millis() / 500) % 2;
  bool blinkFast = (millis() / 100) % 2;
  switch (status)
  {
  case LedStatus::WIFI_SETUP:
    setLed(false, false, blinkSlow);
    break;
  case LedStatus::WIFI_CONNECTING:
    setLed(false, false, true);
    break;
  case LedStatus::COMPANION_CONNECTING:
    setLed(false, blinkSlow, blinkSlow);
    break;
  case LedStatus::READY:
    setLed(false, true, false);
    break;
  case LedStatus::REJECTED:
    setLed(true, blinkSlow, blinkSlow);
    break;
  case LedStatus::WIFI_RESET_HOLD:
    setLed(blinkFast, blinkFast, blinkFast);
    break;
  }
}

// Where things stand, for the LED: WiFi first, then the connection to Companion.
LedStatus connectionLedStatus()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return LedStatus::WIFI_CONNECTING;
  }
  switch (satellite.getConnectionState())
  {
  case SatelliteConnectionState::READY:
    return surface.isRegistered() ? LedStatus::READY : LedStatus::COMPANION_CONNECTING;
  case SatelliteConnectionState::REJECTED:
    return LedStatus::REJECTED;
  default:
    return LedStatus::COMPANION_CONNECTING;
  }
}

// A message in the middle of the screen, for while there are no keys to show.
void drawStatusScreen(const String &title, const String &line1 = "", const String &line2 = "", const String &line3 = "")
{
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextSize(2);
  tft.drawString(title, kScreenWidth / 2, kScreenHeight / 2 - 40);
  tft.setTextSize(1);
  tft.drawString(line1, kScreenWidth / 2, kScreenHeight / 2);
  tft.drawString(line2, kScreenWidth / 2, kScreenHeight / 2 + 16);
  tft.drawString(line3, kScreenWidth / 2, kScreenHeight / 2 + 40);
}

// Until Companion has the surface, show what the board is waiting for instead of empty keys.
void drawCompanionStatus(SatelliteConnectionState state)
{
  String self = "This device: " + WiFi.localIP().toString();
  if (state == SatelliteConnectionState::REJECTED)
  {
    drawStatusScreen("Companion too old", "This needs Companion 4.3 or later", self);
  }
  else if (satellite.getTargetHostname().length() == 0)
  {
    drawStatusScreen("Companion not set", "Add this device in Companion (Surfaces > Discover),",
                     "or hold BOOT for 3s to set it in WiFi setup", self);
  }
  else
  {
    drawStatusScreen("Looking for Companion",
                     satellite.getTargetHostname() + ":" + String(satellite.getTargetPort()), self);
  }
}

void drawEmptyGrid()
{
  tft.fillScreen(TFT_BLACK);
  for (uint8_t i = 0; i < kKeyCount; i++)
  {
    drawCellColor(i, TFT_BLACK);
  }
}

// Join the saved WiFi network, or run the setup portal until someone sets one up.
void setupWiFi()
{
  // A name per board, from the end of its MAC address
  char apNameBuffer[24];
  snprintf(apNameBuffer, sizeof(apNameBuffer), "CYD-Companion-%04X", static_cast<unsigned>((ESP.getEfuseMac() >> 32) & 0xffff));
  String apName = apNameBuffer;

  drawStatusScreen("Connecting to WiFi", "Hold BOOT for 3s to set up another network");
  showLedStatus(LedStatus::WIFI_CONNECTING);

  wifiManager.addParameter(&companionParam);
  std::vector<const char *> menu = {"wifi", "exit"};
  wifiManager.setMenu(menu);
  wifiManager.setTitle("Companion CYD");
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(kWifiSetupTimeoutSeconds);
  wifiManager.setAPCallback([apName](WiFiManager *)
                            {
                              drawStatusScreen("WiFi setup", "On your phone, join the WiFi network",
                                               apName, "then open http://192.168.4.1 if it doesn't open itself");
                              showLedStatus(LedStatus::WIFI_SETUP); });
  wifiManager.setSaveParamsCallback([]()
                                    { companionFromSetup = companionParam.getValue(); });

  if (!wifiManager.autoConnect(apName.c_str()))
  {
    // Nobody set it up in time: start again, which tries the saved network first
    Serial.println("WiFi setup timed out, restarting");
    ESP.restart();
  }
  Serial.printf("Connected to WiFi, IP %s\n", WiFi.localIP().toString().c_str());
  // Turn off WiFi power saving, so Companion's updates arrive straight away. With it on,
  // the radio only wakes every few beacons to collect them, adding up to a few hundred
  // ms, and on some access points they stall until the connection times out.
  WiFi.setSleep(false);
}

// Point the satellite at what was entered on the setup page, if anything ("host" or "host:port").
void applyCompanionFromSetup()
{
  companionFromSetup.trim();
  if (companionFromSetup.length() == 0)
  {
    return;
  }
  String host = companionFromSetup;
  uint16_t port = 16622;
  int colon = host.lastIndexOf(':');
  if (colon > 0)
  {
    long parsed = host.substring(colon + 1).toInt();
    if (parsed > 0 && parsed <= 65535)
    {
      port = static_cast<uint16_t>(parsed);
      host = host.substring(0, colon);
    }
  }
  satellite.setTarget(host, port);
}

// BOOT: a short press runs the touch calibration; holding it forgets the WiFi
// network and restarts into setup. Returns true while it's being held, so the
// LED can show the countdown.
bool handleBootButton()
{
  static uint32_t pressedAtMs = 0;
  static int lastSecondsLeft = -1;
  bool down = digitalRead(BOOT_BUTTON) == LOW;
  uint32_t now = millis();

  if (down)
  {
    if (pressedAtMs == 0)
    {
      pressedAtMs = now;
    }
    uint32_t heldMs = now - pressedAtMs;
    if (heldMs >= kBootWifiResetMs)
    {
      Serial.println("Forgetting the WiFi network and restarting into setup");
      drawStatusScreen("Forgetting WiFi", "Restarting into WiFi setup...");
      wifiManager.resetSettings();
      delay(500);
      ESP.restart();
    }
    if (heldMs >= kBootShortPressMs)
    {
      int secondsLeft = (kBootWifiResetMs - heldMs + 999) / 1000;
      if (secondsLeft != lastSecondsLeft)
      {
        keysHidden = true;
        drawStatusScreen("Keep holding to forget WiFi", String(secondsLeft) + "...", "Let go to cancel");
        lastSecondsLeft = secondsLeft;
      }
      return true;
    }
    return false;
  }

  if (pressedAtMs != 0)
  {
    bool shortPress = now - pressedAtMs < kBootShortPressMs;
    pressedAtMs = 0;
    lastSecondsLeft = -1;
    keysHidden = false;
    if (shortPress)
    {
      runTouchCalibration();
    }
    // The calibration or the countdown replaced the keys: start the screen again, and
    // reconnect so Companion sends every key again
    if (satellite.getConnectionState() == SatelliteConnectionState::READY)
    {
      drawEmptyGrid();
      satellite.setTarget(satellite.getTargetHostname(), satellite.getTargetPort());
    }
    else
    {
      drawCompanionStatus(satellite.getConnectionState());
    }
    activeKeyIndex = -1;
  }
  return false;
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  tft.init();
  tft.setRotation(1); // landscape; try 3 if the display looks upside down
  // Bitmaps are drawn from native (little-endian) RGB565 uint16_t values;
  // pushImage() sends them high byte first unless told to swap.
  tft.setSwapBytes(true);
  dmaReady = tft.initDMA();
  tft.fillScreen(TFT_BLACK);

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(1);
  loadTouchCalibration();
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  setupWiFi();
  // Set before begin(), so it replaces whatever target was saved
  applyCompanionFromSetup();

  // Until Companion has the surface, the screen says what the board is waiting for
  satellite.setOnConnectionStateChange([](SatelliteConnectionState state)
                                       {
                                         if (state != SatelliteConnectionState::READY)
                                         {
                                           drawCompanionStatus(state);
                                         } });
  surface.setOnRegistered([]()
                          { drawEmptyGrid(); });

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
                                if (state.index >= kKeyCount || keysHidden)
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
                           if (!keysHidden)
                           {
                             drawEmptyGrid();
                           } });

  drawCompanionStatus(satellite.getConnectionState());
}

void loop()
{
  // Draws any key updates received since last time (see drawCellBitmap).
  satellite.loop();

  bool bootHeld = handleBootButton();
  showLedStatus(bootHeld ? LedStatus::WIFI_RESET_HOLD : connectionLedStatus());

  if (touchscreen.touched() && surface.isRegistered())
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
