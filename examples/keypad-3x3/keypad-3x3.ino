// Companion Microsatellite - 3x3 keypad.
//
// A 9-key surface: nine push buttons in a 3x3 grid, each with an APA102
// ("DotStar") LED that shows the key's color from Companion. The surface's
// brightness setting dims the LEDs, and they go dark while the connection to
// Companion is down, so they never show stale colors.
//
// Written for an ESP32-C3 board (the pins below don't all exist on other
// ESP32 variants - change them to suit your wiring).
//
// Library dependencies (install via Arduino Library Manager):
// - ESP Async WebServer and Async TCP (both by ESP32Async)
// - ArduinoJson
// - Adafruit DotStar

#include <Arduino.h>

#include <Adafruit_DotStar.h>
#include <SPI.h>
#include <WiFi.h>

#include "companion-satellite.hpp"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

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

// The last debounced state of each button, and when its pin last changed.
bool keyPressed[kKeyCount] = {};
bool lastReading[kKeyCount] = {};
uint32_t lastChangeMs[kKeyCount] = {};

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

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Turn off WiFi power saving, so Companion's updates arrive straight away. With it on,
  // the radio only wakes every few beacons to collect them, adding up to a few hundred
  // ms, and on some access points they stall until the connection times out.
  WiFi.setSleep(false);
  // Many small ESP32-C3 boards have a poorly matched antenna and connect more
  // reliably at reduced transmit power.
  WiFi.setTxPower(WIFI_POWER_13dBm);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.printf("Connected to WiFi, IP %s\n", WiFi.localIP().toString().c_str());

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
                               strip.setBrightness(map(brightness, 0, 100, 0, 255));
                               strip.show(); });

  // Go dark while disconnected. Companion sends every key's color again once the
  // surface is registered after a reconnect.
  satellite.setOnConnectionStateChange([](SatelliteConnectionState state)
                                       {
                                         if (state != SatelliteConnectionState::READY)
                                         {
                                           strip.clear();
                                           strip.show();
                                         } });

  satellite.begin();
}

void loop()
{
  // Runs the callbacks above on this task, as updates arrive.
  satellite.loop();
  readButtons();
  delay(1);
}
