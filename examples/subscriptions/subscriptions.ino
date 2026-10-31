// Companion Microsatellite - button subscription example.
//
// Demonstrates the button *subscription* API as an alternative to
// registering a full SatelliteSurface. A subscription lets this device
// observe - and interact with - a single button at an absolute
// page/row/column location in Companion, without the device appearing as
// its own entry in Companion's Surfaces list.
//
// Subscriptions are registered automatically whenever the connection to
// Companion becomes ready, including after a reconnect - just create the
// SatelliteSubscription object and leave it alive.
//
// IMPORTANT - this must be enabled in Companion:
// Button subscriptions are an optional server capability, not something
// this device can turn on by itself. Check Companion's settings for this
// Satellite connection and make sure subscription support is turned on
// there, or supportsSubscriptions() below will return false.
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

#define BUTTON_PIN 0

Satellite satellite("microsatellite-subscriptions");

// Follow the button at page 1, row 0, column 0 - change this to a button
// that actually exists in your Companion configuration.
SatelliteSubscriptionConfig subscriptionConfig = {
    .colors = true,
    .text = true,
};
SatelliteSubscription subscription(satellite, "1/0/0", subscriptionConfig);

bool lastButtonPressed = false;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  pinMode(BUTTON_PIN, INPUT_PULLUP);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  // Turn off WiFi power saving, so Companion's updates arrive straight away. With it on,
  // the radio only wakes every few beacons to collect them, adding up to a few hundred
  // ms, and on some access points they stall until the connection times out.
  WiFi.setSleep(false);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    Serial.println("Connecting to WiFi...");
  }
  Serial.println("Connected to WiFi");

  satellite.setOnConnectionStateChange([](SatelliteConnectionState state)
                                       {
                                         Serial.printf("Connection: %s\n", satelliteConnectionStateName(state));
                                         if (state == SatelliteConnectionState::READY && !satellite.supportsSubscriptions())
                                         {
                                           Serial.println("Companion has button subscriptions disabled - enable them in Companion's settings");
                                         } });

  // Fires once after subscribing, and again whenever the button changes in Companion.
  subscription.setOnStateChange([](const SatelliteControlState &state)
                                {
                                  Serial.printf("%s: pressed=%d text=%s\n",
                                                state.location.value_or("?").c_str(),
                                                state.pressed.value_or(false),
                                                state.text.value_or("<none>").c_str());
                                  if (state.keyColor.has_value())
                                  {
                                    Serial.printf("  color=#%02x%02x%02x\n", state.keyColor->r, state.keyColor->g, state.keyColor->b);
                                  } });

  // Eg. a location that doesn't exist.
  subscription.setOnError([](const String &message)
                          { Serial.printf("Subscription rejected: %s\n", message.c_str()); });

  satellite.begin();
}

void loop()
{
  satellite.loop();

  // Report the physical button through the subscription.
  bool pressed = digitalRead(BUTTON_PIN) == LOW;
  if (pressed != lastButtonPressed)
  {
    lastButtonPressed = pressed;
    subscription.action(pressed ? SatelliteAction::PRESSED : SatelliteAction::RELEASED);
  }

  delay(10);
}
