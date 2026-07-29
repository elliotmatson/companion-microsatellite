// Companion Microsatellite - pincode lock example.
//
// Demonstrates a surface that handles Companion's pincode-lock feature
// itself. When a page protected by a pincode is displayed on this surface,
// Companion reports the lock state via setOnLockedStateSet() instead of
// streaming normal button state, and this device is responsible for both
// showing that it's locked and forwarding digit presses back with
// pressPincodeKey().
//
// Two levels of support:
//  - FULL    - this device displays the locked state AND lets the user
//              enter the pincode on it (what this example uses).
//  - PARTIAL - this device can display the locked state, but has no way
//              for the user to enter a pincode on it (e.g. no keypad);
//              Companion assumes the pincode is entered elsewhere.
//
// Between a LOCKED=true and the matching LOCKED=false, Companion will not
// send any other drawing or accept any other input for this device - so
// this example ignores KEY-STATE updates while locked.
//
// Library dependencies (install via Arduino Library Manager):
// - ESPAsyncWebServer (ESP32Async)
// - AsyncTCP (ESP32Async)
// - ArduinoJson

#include <Arduino.h>
#include <WiFi.h>
#include "companion-satellite.hpp"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

Satellite satellite("microsatellite-pincode");

// Requesting full pincode handling - see PARTIAL note above.
SatelliteSurfaceConfig surfaceConfig = {
    .totalKeys = 4,
    .keysPerRow = 2,
    .sendColors = true,
    .sendText = true,
    .pincodeSupport = SatelliteSurfacePincodeSupport::FULL,
};

SatelliteSurface surface(satellite, "Pincode Demo Surface", surfaceConfig);

// Tracks the lock state reported by Companion, so KEY-STATE handling can
// ignore stale updates while locked.
bool locked = false;

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

  // While locked, Companion won't send meaningful KEY-STATE updates.
  surface.setOnKeyStateChange([](const SatelliteSurfaceKeyState &state)
                              {
                                if (locked)
                                {
                                  return;
                                }
                                Serial.printf("onKeyStateChange: index=%u text=%s\n", state.index, state.text.value_or("<none>").c_str());
                              });

  // Companion -> device: reports when a pincode-protected page is
  // locked/unlocked, and how many digits have been entered so far.
  surface.setOnLockedStateSet([](bool isLocked, uint8_t charactersEntered)
                              {
                                locked = isLocked;
                                Serial.printf("onLockedStateSet: locked=%d charactersEntered=%u\n", isLocked, charactersEntered);
                              });

  // Demonstrate entering a pincode, one digit at a time.
  delay(500);
  const uint8_t digits[] = {1, 2, 3, 4};
  for (uint8_t digit : digits)
  {
    surface.pressPincodeKey(digit);
    Serial.printf("pressPincodeKey(%u)\n", digit);
    delay(300);
  }
}

void loop()
{
  delay(1000);
}
