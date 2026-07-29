// Exhaustive feature-coverage example for the CompanionSatellite library.
//
// Registers every callback the library offers, then runs through every
// public method on Satellite and SatelliteSurface once at boot. Intended as
// a reference for what's available, not as an interactive tool - watch the
// serial log to see what each call does.

#include <Arduino.h>
#include <WiFi.h>
#include "companion-satellite.hpp"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Satellite instance, using a non-default API port to exercise that constructor path.
Satellite satellite("exhaustive-test", 9998);

// Input variable: a value this device produces.
SatelliteSurfaceVariable inputVariable = {
    .id = "input1",
    .type = SatelliteSurfaceVariableType::VARIABLE_INPUT,
    .name = "Input 1",
    .description = "Test input variable"};

// Output variable: a value Companion produces.
SatelliteSurfaceVariable outputVariable = {
    .id = "output1",
    .type = SatelliteSurfaceVariableType::VARIABLE_OUTPUT,
    .name = "Output 1",
    .description = "Test output variable"};

// Surface config exercising every option: bitmaps, colors, text, brightness, pincode, config fields, page nav.
SatelliteSurfaceConfig fullConfig = {
    .totalKeys = 8,
    .keysPerRow = 4,
    .sendColors = true,
    .sendText = true,
    .sendTextStyle = true,
    .supportsBrightness = true,
    .pincodeSupport = SatelliteSurfacePincodeSupport::FULL,
    .configFieldsJson = R"([{"id":"demo_field","type":"textinput","label":"Demo Field"}])",
    .canChangePageLabel = "Allow this device to change pages",
    .bitmapSize = 90, // a custom size, rather than the 72px default (max safe size for the default 32KB MAX_RX_BUFFER_SIZE)
    .bitmapFormat = "png",
};

SatelliteSurface surface(satellite, "Exhaustive Surface", fullConfig, {inputVariable, outputVariable});

const char *kSubId = "exhaustive-sub-1";

void setup()
{
  Serial.begin(115200);
  delay(1000);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    ESP_LOGI(__func__, "Connecting to WiFi...");
  }
  ESP_LOGI(__func__, "Connected to WiFi");

  satellite.begin();

  // Register every callback the library exposes; each just logs what it received.
  surface.setOnKeyStateChange([](const SatelliteSurfaceKeyState &state)
                              {
                                ESP_LOGI("onKeyStateChange", "index=%u type=%d pressed=%d text=%s location=%s",
                                         state.index, static_cast<int>(state.type),
                                         state.pressed.value_or(false),
                                         state.text.value_or("<none>").c_str(),
                                         state.location.value_or("<none>").c_str()); });

  surface.setOnKeysClear([]()
                         { ESP_LOGI("onKeysClear", "keys cleared"); });

  surface.setOnBrightnessSet([](uint8_t brightness)
                             { ESP_LOGI("onBrightnessSet", "brightness=%u", brightness); });

  surface.setOnVariableSet([](String id, String value)
                           { ESP_LOGI("onVariableSet", "id=%s value=%s", id.c_str(), value.c_str()); });

  surface.setOnLockedStateSet([](bool locked, uint8_t charactersEntered)
                              { ESP_LOGI("onLockedStateSet", "locked=%d charactersEntered=%u", locked, charactersEntered); });

  surface.setOnDeviceConfig([](const String &configJson)
                            { ESP_LOGI("onDeviceConfig", "config=%s", configJson.c_str()); });

  satellite.setOnSubscriptionState([](const Satellite::SubscriptionState &state)
                                   {
                                     ESP_LOGI("onSubscriptionState", "subId=%s type=%d pressed=%d text=%s location=%s",
                                              state.subId.c_str(), static_cast<int>(state.type),
                                              state.pressed.value_or(false),
                                              state.text.value_or("<none>").c_str(),
                                              state.location.value_or("<none>").c_str()); });

  // Wait for protocol negotiation before exercising anything that needs it.
  unsigned long deadline = millis() + 10000;
  while (!satellite.isReady() && millis() < deadline)
  {
    delay(50);
  }

  if (!satellite.isReady())
  {
    ESP_LOGW(__func__, "Satellite did not become ready in time, skipping the feature walkthrough");
    return;
  }

  ESP_LOGI(__func__, "deviceName=%s hostname=%s companionVersion=%s satelliteApiVersion=%s",
           satellite.getDeviceName().c_str(), satellite.getHostname().c_str(),
           satellite.getCompanionVersion().c_str(), satellite.getSatelliteApiVersion().c_str());

  // Surface actions.
  surface.buttonAction(0, SatelliteAction::PRESSED);
  delay(200);
  surface.buttonAction(0, SatelliteAction::RELEASED);
  delay(200);
  surface.buttonAction(1, SatelliteAction::RIGHT);
  delay(200);
  surface.buttonAction(1, SatelliteAction::LEFT);

  surface.setVariable("output1", "hello from setup");

  surface.pressPincodeKey(1);
  delay(200);
  surface.pressPincodeKey(2);

  surface.reportFirmwareUpdateInfo("https://example.com/firmware.bin");
  delay(200);
  surface.reportFirmwareUpdateInfo(""); // clear it again

  surface.changePage(SatellitePageDirection::NEXT);
  delay(200);
  surface.changePage(SatellitePageDirection::PREVIOUS);

  // Button subscriptions - an alternative to a full surface, so this is a
  // separate location/id, not tied to the surface above. Needs to be
  // enabled for this connection in Companion's settings.
  if (satellite.supportsSubscriptions())
  {
    satellite.addSubscription(kSubId, "1/0/0", 72, String("png"), true, true, true);
    delay(200);
    satellite.subscriptionAction(kSubId, SatelliteAction::PRESSED);
    delay(200);
    satellite.subscriptionAction(kSubId, SatelliteAction::RELEASED);
    delay(200);
    satellite.subscriptionAction(kSubId, SatelliteAction::RIGHT);
    delay(200);
    satellite.subscriptionAction(kSubId, SatelliteAction::LEFT);
    delay(200);
    satellite.removeSubscription(kSubId);
  }
  else
  {
    ESP_LOGW(__func__, "Companion did not advertise subscription support - skipping the subscription walkthrough");
  }
}

void loop()
{
  delay(1000);
}
