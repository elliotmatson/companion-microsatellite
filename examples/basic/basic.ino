// Companion Microsatellite - basic hardware demo.
//
// A single-button virtual surface backed by a physical button and a
// NeoPixel: the NeoPixel mirrors the key color/brightness Companion sends,
// and pressing the button reports a press/release back to Companion.
//
// Hardware: a NeoPixel on pin 18, a button on pin 0.
//
// Library dependencies (install via Arduino Library Manager):
// - ESPAsyncWebServer (ESP32Async)
// - AsyncTCP (ESP32Async)
// - ArduinoJson
// - Adafruit NeoPixel

#include <Arduino.h>

#include <Preferences.h>

#include "companion-satellite.hpp"
#include "WiFi.h"

#include "Adafruit_NeoPixel.h"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define BUTTON_PIN 0
#define NEOPIXEL_PIN 18
#define NEOPIXEL_COUNT 1

Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

Satellite satellite("microsatellite");

SatelliteSurfaceConfig surfaceConfig = {
    .totalKeys = 1,
    .keysPerRow = 1,
    .sendColors = true,
    .supportsBrightness = true,
};

SatelliteSurface surface(satellite, "Surface 1", surfaceConfig);

TaskHandle_t buttonTaskHandle = NULL;

// The GPIO interrupt only notifies this task; the actual buttonAction() call
// (which takes a mutex, allocates a String, and writes to the TCP socket)
// happens here in task context, since none of that is safe to do directly
// from an ISR.
void buttonTask(void *parameter)
{
  for (;;)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool pressed = !digitalRead(BUTTON_PIN);
    surface.buttonAction(0, pressed ? SatelliteAction::PRESSED : SatelliteAction::RELEASED);
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  satellite.begin();
  Serial.println("Companion Satellite initialized");

  xTaskCreate(
      buttonTask,
      "Button Task",
      2048,
      NULL,
      1,
      &buttonTaskHandle);

  // The ISR only notifies buttonTask, it doesn't call into the library
  // directly (see buttonTask for why).
  attachInterrupt(BUTTON_PIN, [](void)
                  { vTaskNotifyGiveFromISR(buttonTaskHandle, NULL); }, CHANGE);

  strip.begin();
  strip.show();

  // Update the NeoPixel color to match Companion's key color.
  surface.setOnKeyStateChange([&](const SatelliteSurfaceKeyState &state)
                              {
                                if (state.index >= NEOPIXEL_COUNT || !state.keyColor.has_value())
                                {
                                  return;
                                }
                                strip.setPixelColor(state.index, strip.Color(state.keyColor->r, state.keyColor->g, state.keyColor->b));
                                strip.show(); });

  // Update the NeoPixel brightness to match Companion's brightness (0-100 -> 0-255).
  surface.setOnBrightnessSet([&](uint8_t brightness)
                             {
                                strip.setBrightness(map(brightness, 0, 100, 0, 255));
                                strip.show(); });
}

// Button handling happens in buttonTask, so the loop task isn't needed.
void loop()
{
  vTaskDelete(NULL);
}
