// Companion Microsatellite - button subscription example.
//
// Demonstrates the button *subscription* API as an alternative to
// registering a full SatelliteSurface. A subscription lets this device
// observe - and interact with - a single button at an absolute
// page/row/column location in Companion, without the device appearing as
// its own entry in Companion's Surfaces list.
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

// Location to subscribe to, in "page/row/column" form - change this to a
// button that actually exists in your Companion configuration.
const char *kLocation = "1/0/0";

// Client-chosen id for this subscription; must be unique per connection.
const char *kSubId = "microsatellite-sub-1";

Satellite satellite("microsatellite-subscriptions");

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

  // Fires once immediately after a successful subscribe, and again whenever
  // the subscribed button's state changes in Companion.
  satellite.setOnSubscriptionState([](const Satellite::SubscriptionState &state)
                                   {
                                     Serial.printf("subId=%s pressed=%d text=%s\n",
                                                    state.subId.c_str(),
                                                    state.pressed.value_or(false),
                                                    state.text.value_or("<none>").c_str());
                                     if (state.keyColor.has_value())
                                     {
                                       Serial.printf("  color=#%02x%02x%02x\n",
                                                     state.keyColor->r, state.keyColor->g, state.keyColor->b);
                                     }
                                   });

  // Give protocol negotiation a moment to complete before checking capabilities.
  unsigned long deadline = millis() + 10000;
  while (!satellite.isReady() && millis() < deadline)
  {
    delay(50);
  }

  if (!satellite.isReady())
  {
    Serial.println("Satellite did not become ready in time");
    return;
  }

  if (!satellite.supportsSubscriptions())
  {
    Serial.println("Companion did not advertise subscription support - enable it in Companion's connection settings");
    return;
  }

  satellite.addSubscription(kSubId, kLocation, 72, String("rgb"), true, true, true);
  Serial.printf("Subscribed to %s\n", kLocation);

  // Demonstrate reporting input back through the subscription.
  delay(500);
  satellite.subscriptionAction(kSubId, SatelliteAction::PRESSED);
  delay(200);
  satellite.subscriptionAction(kSubId, SatelliteAction::RELEASED);
}

void loop()
{
  delay(1000);
}
