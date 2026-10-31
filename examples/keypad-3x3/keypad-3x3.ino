// Companion Microsatellite - 3x3 keypad.
//
// A 9-key surface: nine push buttons in a 3x3 grid, each with an APA102
// ("DotStar") LED that shows the key's color from Companion. The surface's
// brightness setting dims the LEDs.
//
// WiFi is set up from a phone: with no saved network (or if it can't be
// reached) the board starts its own access point with a setup page, where
// you pick the network and can enter Companion's address. Until Companion
// has the surface, the LEDs show where it's got to. To forget the WiFi
// network, hold the top-left key while the board starts.
//
// Written for an ESP32-C3 board (the pins below don't all exist on other
// ESP32 variants - change them to suit your wiring).
//
// It's too big for the default partition scheme: choose "Huge APP (3MB No
// OTA/1MB SPIFFS)" under Tools > Partition Scheme (PlatformIO:
// board_build.partitions = huge_app.csv).
//
// Library dependencies (install via Arduino Library Manager):
// - ESP Async WebServer and Async TCP (both by ESP32Async)
// - ArduinoJson
// - Adafruit DotStar
// - WiFiManager (tzapu), 2.0 or later

#include <Arduino.h>

#include <Adafruit_DotStar.h>
#include <SPI.h>
#include <Ticker.h>
#include <WiFi.h>
#include <WiFiManager.h>

#include "companion-satellite.hpp"

// How long the WiFi setup network stays up before the board restarts and tries the saved network again
constexpr uint32_t kWifiSetupTimeoutSeconds = 300;

// How long to hold the top-left key while the board starts to forget the WiFi network
constexpr uint32_t kWifiResetHoldMs = 3000;

constexpr uint8_t kKeyCount = 9;
constexpr uint8_t kKeysPerRow = 3;

// Each button connects its pin to ground, in key order: left to right, top to
// bottom. GPIO 9 is the C3's BOOT strapping pin, so don't hold that key while
// the board resets.
constexpr uint8_t kButtonPins[kKeyCount] = {1, 0, 5, 6, 7, 9, 10, 20, 21};

// The LED chain runs right to left along each row, so key 0 (top left) is LED 2.
constexpr uint8_t kLedForKey[kKeyCount] = {2, 1, 0, 5, 4, 3, 8, 7, 6};

#define LED_DATA_PIN 3
#define LED_CLOCK_PIN 4

// Ignore a button's changes for this long after it last changed (contact bounce).
constexpr uint32_t kDebounceMs = 20;

// Status lights: how bright (out of 255 - full brightness is glaring), and the
// key that shows a status on its own.
constexpr uint8_t kStatusLevel = 32;
constexpr uint8_t kStatusKey = 4; // the middle key

Adafruit_DotStar strip(kKeyCount, LED_DATA_PIN, LED_CLOCK_PIN, DOTSTAR_BGR);

Satellite satellite("microsatellite");

// Colors only: no bitmaps or text, so there's very little for Companion to send.
SatelliteSurfaceConfig surfaceConfig = {
    .totalKeys = kKeyCount,
    .keysPerRow = kKeysPerRow,
    .sendColors = true,
    .sendText = false,
    .supportsBrightness = true,
};

SatelliteSurface surface(satellite, "3x3 Keypad", surfaceConfig);

// WiFi setup: a captive portal on the board's own access point. Besides the
// network, it asks (optionally) for Companion's address, as "host" or
// "host:port" - leave it empty to keep the current one, or to let Companion
// find this device itself (Surfaces > Discover).
WiFiManager wifiManager;
WiFiManagerParameter companionParam("companion", "Companion IP or hostname (optional)", "", 64);
String companionFromSetup; // what was entered, applied once the satellite starts

// Blinks the LEDs while the setup portal is up, as it doesn't return until it's done
Ticker setupBlinker;

// Companion's brightness for the strip (0-255), restored once the status lights are done
uint8_t companionBrightness = 255;

// The last debounced state of each button, and when its pin last changed.
bool keyPressed[kKeyCount] = {};
bool lastReading[kKeyCount] = {};
uint32_t lastChangeMs[kKeyCount] = {};

// What the LEDs show until Companion has the surface. (Up here, ahead of every
// function, so the prototypes the Arduino IDE generates for them can use it.)
enum class Status
{
  WIFI_CONNECTING,      // middle key blue: joining the WiFi network
  WIFI_SETUP,           // every key blue, blinking: setup access point is up
  COMPANION_CONNECTING, // middle key cyan, blinking: on WiFi, looking for Companion
  REJECTED,             // middle key red, blinking: Companion is too old for this library
  WIFI_RESET_HOLD,      // every key white, blinking fast: about to forget WiFi
};

void showStatus(Status status)
{
  uint32_t color = 0;
  bool allKeys = false;
  uint32_t blinkMs = 500;
  switch (status)
  {
  case Status::WIFI_CONNECTING:
    color = strip.Color(0, 0, kStatusLevel);
    blinkMs = 0;
    break;
  case Status::WIFI_SETUP:
    color = strip.Color(0, 0, kStatusLevel);
    allKeys = true;
    break;
  case Status::COMPANION_CONNECTING:
    color = strip.Color(0, kStatusLevel, kStatusLevel);
    break;
  case Status::REJECTED:
    color = strip.Color(kStatusLevel, 0, 0);
    break;
  case Status::WIFI_RESET_HOLD:
    color = strip.Color(kStatusLevel, kStatusLevel, kStatusLevel);
    allKeys = true;
    blinkMs = 100;
    break;
  }
  if (blinkMs != 0 && (millis() / blinkMs) % 2)
  {
    color = 0;
  }

  strip.setBrightness(255);
  strip.clear();
  if (allKeys)
  {
    strip.fill(color);
  }
  else
  {
    strip.setPixelColor(kLedForKey[kStatusKey], color);
  }
  strip.show();
}

// Where things stand: WiFi first, then the connection to Companion.
Status connectionStatus()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    return Status::WIFI_CONNECTING;
  }
  if (satellite.getConnectionState() == SatelliteConnectionState::REJECTED)
  {
    return Status::REJECTED;
  }
  return Status::COMPANION_CONNECTING;
}

// Holding the top-left key while the board starts forgets the WiFi network, so
// the setup portal opens. Let go before kWifiResetHoldMs to cancel.
void checkWifiResetHold()
{
  uint32_t pressedAtMs = millis();
  while (digitalRead(kButtonPins[0]) == LOW)
  {
    if (millis() - pressedAtMs >= kWifiResetHoldMs)
    {
      Serial.println("Forgetting the WiFi network");
      wifiManager.resetSettings();
      return;
    }
    showStatus(Status::WIFI_RESET_HOLD);
    delay(10);
  }
}

// Join the saved WiFi network, or run the setup portal until someone sets one up.
void setupWiFi()
{
  // A name per board, from the end of its MAC address
  char apNameBuffer[24];
  snprintf(apNameBuffer, sizeof(apNameBuffer), "Keypad-Companion-%04X", static_cast<unsigned>((ESP.getEfuseMac() >> 32) & 0xffff));
  String apName = apNameBuffer;

  showStatus(Status::WIFI_CONNECTING);

  WiFi.mode(WIFI_STA);
  // Many small ESP32-C3 boards have a poorly matched antenna and connect more
  // reliably at reduced transmit power.
  WiFi.setTxPower(WIFI_POWER_13dBm);

  wifiManager.addParameter(&companionParam);
  std::vector<const char *> menu = {"wifi", "exit"};
  wifiManager.setMenu(menu);
  wifiManager.setTitle("Companion Keypad");
  wifiManager.setConnectTimeout(20);
  wifiManager.setConfigPortalTimeout(kWifiSetupTimeoutSeconds);
  wifiManager.setAPCallback([apName](WiFiManager *)
                            {
                              Serial.printf("WiFi setup: join the WiFi network %s, then open http://192.168.4.1 if it doesn't open itself\n", apName.c_str());
                              setupBlinker.attach_ms(100, []()
                                                     { showStatus(Status::WIFI_SETUP); }); });
  wifiManager.setSaveParamsCallback([]()
                                    { companionFromSetup = companionParam.getValue(); });

  bool connected = wifiManager.autoConnect(apName.c_str());
  setupBlinker.detach();
  if (!connected)
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

// Send a press or release for each button whose state has settled on a new value.
void readButtons()
{
  uint32_t now = millis();
  for (uint8_t i = 0; i < kKeyCount; i++)
  {
    bool reading = digitalRead(kButtonPins[i]) == LOW;
    if (reading != lastReading[i])
    {
      lastReading[i] = reading;
      lastChangeMs[i] = now;
    }
    else if (reading != keyPressed[i] && now - lastChangeMs[i] >= kDebounceMs)
    {
      keyPressed[i] = reading;
      surface.buttonAction(i, reading ? SatelliteAction::PRESSED : SatelliteAction::RELEASED);
    }
  }
}

void setup()
{
  Serial.begin(115200);

  for (uint8_t pin : kButtonPins)
  {
    pinMode(pin, INPUT_PULLUP);
  }

  strip.begin();
  strip.show(); // all off

  checkWifiResetHold();
  setupWiFi();
  // Set before begin(), so it replaces whatever target was saved
  applyCompanionFromSetup();

  // Once Companion has the surface, the LEDs are its: start dark, and Companion
  // sends every key's color.
  surface.setOnRegistered([]()
                          {
                            strip.clear();
                            strip.setBrightness(companionBrightness);
                            strip.show(); });

  // Show each key's color from Companion.
  surface.setOnKeyStateChange([](const SatelliteControlState &state)
                              {
                                if (state.index >= kKeyCount || !state.keyColor)
                                {
                                  return;
                                }
                                strip.setPixelColor(kLedForKey[state.index], strip.Color(state.keyColor->r, state.keyColor->g, state.keyColor->b));
                                strip.show(); });

  // Companion's brightness is 0-100, the strip's 0-255.
  surface.setOnBrightnessSet([](uint8_t brightness)
                             {
                               companionBrightness = map(brightness, 0, 100, 0, 255);
                               strip.setBrightness(companionBrightness);
                               strip.show(); });

  satellite.begin();
}

void loop()
{
  // Runs the callbacks above on this task, as updates arrive.
  satellite.loop();

  if (surface.isRegistered())
  {
    readButtons();
  }
  else
  {
    showStatus(connectionStatus());
  }
  delay(1);
}
