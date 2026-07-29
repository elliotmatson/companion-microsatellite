// Companion Microsatellite - custom config fields and variables example.
//
// Demonstrates the two ways data moves between this device and Companion
// outside of button presses:
//
//  - CONFIG_FIELDS / DEVICE-CONFIG: a small custom settings form that
//    Companion renders in its surface settings panel (Companion -> device).
//    Companion pushes the current values down via setOnDeviceConfig() once
//    right after the device is registered, and again every time the user
//    edits a field.
//
//  - Variables: named values attached to a surface. An "input" variable is
//    a value THIS DEVICE produces - push updates with setVariable()
//    (device -> Companion). An "output" variable is a value COMPANION
//    produces - received in setOnVariableSet() (Companion -> device).
//
// This example defines one config field (a report interval) and one of
// each variable type: an input variable reporting uptime on that interval,
// and an output variable that just logs whatever Companion sends it.
//
// Library dependencies (install via Arduino Library Manager):
// - ESPAsyncWebServer (ESP32Async)
// - AsyncTCP (ESP32Async)
// - ArduinoJson

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include "companion-satellite.hpp"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

Satellite satellite("microsatellite-config");

// Input variable: a value this device produces, pushed to Companion with setVariable().
SatelliteSurfaceVariable uptimeVariable = {
    .id = "uptime_seconds",
    .type = SatelliteSurfaceVariableType::VARIABLE_INPUT,
    .name = "Uptime (seconds)",
    .description = "Seconds since this device booted, reported on the interval set in Companion"};

// Output variable: a value Companion produces, received in setOnVariableSet().
SatelliteSurfaceVariable greetingVariable = {
    .id = "greeting",
    .type = SatelliteSurfaceVariableType::VARIABLE_OUTPUT,
    .name = "Greeting",
    .description = "Any text value pushed to this variable from a Companion button/variable action"};

// A small custom settings form, rendered in Companion's surface settings
// panel. See assets/satellite-config-fields.schema.json in the Companion
// source for the full field schema (textinput, number, checkbox, etc.).
//
// Note: a raw string literal (R"(...)") would read more naturally here,
// but the Arduino IDE's .ino prototype generator doesn't understand raw
// strings and gets confused by the embedded quotes - use escaped quotes
// in a normal string literal instead (this only matters in .ino files;
// plain .cpp files don't have this problem).
SatelliteSurfaceConfig surfaceConfig = {
    .totalKeys = 1,
    .keysPerRow = 1,
    .sendColors = true,
    .sendText = true,
    .configFieldsJson = "[{\"id\":\"device_label\",\"type\":\"textinput\",\"label\":\"Device Label\",\"default\":\"My Microsatellite\"},{\"id\":\"report_interval_seconds\",\"type\":\"number\",\"label\":\"Uptime report interval (seconds)\",\"default\":10,\"min\":1,\"max\":3600}]",
};

// Only used here to carry the config fields and variables.
SatelliteSurface surface(satellite, "Config Demo Surface", surfaceConfig, {uptimeVariable, greetingVariable});

// How often to report the uptime variable, updated from the report_interval_seconds config field.
unsigned long reportIntervalMs = 10000;
unsigned long lastReportMs = 0;

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

  // Companion -> device: fires once after registration, and again whenever
  // the user edits the fields in Companion's surface settings panel.
  surface.setOnDeviceConfig([](const String &configJson)
                            {
                              Serial.printf("onDeviceConfig: %s\n", configJson.c_str());

                              JsonDocument doc;
                              if (deserializeJson(doc, configJson) != DeserializationError::Ok)
                              {
                                Serial.println("Failed to parse config JSON");
                                return;
                              }

                              if (doc["device_label"].is<const char *>())
                              {
                                Serial.printf("device_label = %s\n", doc["device_label"].as<const char *>());
                              }

                              if (doc["report_interval_seconds"].is<int>())
                              {
                                int seconds = doc["report_interval_seconds"].as<int>();
                                reportIntervalMs = static_cast<unsigned long>(seconds) * 1000UL;
                                Serial.printf("report_interval_seconds = %d\n", seconds);
                              }
                            });

  // Companion -> device: fires whenever a value is pushed to an output variable.
  surface.setOnVariableSet([](String id, String value)
                           { Serial.printf("onVariableSet: %s = %s\n", id.c_str(), value.c_str()); });
}

void loop()
{
  // Device -> Companion: push the input variable on the interval configured
  // via the report_interval_seconds config field (default 10s until
  // Companion sends its DEVICE-CONFIG).
  if (millis() - lastReportMs >= reportIntervalMs)
  {
    lastReportMs = millis();
    String uptimeSeconds = String(millis() / 1000);
    surface.setVariable("uptime_seconds", uptimeSeconds);
    Serial.printf("setVariable(\"uptime_seconds\", \"%s\")\n", uptimeSeconds.c_str());
  }

  delay(50);
}
