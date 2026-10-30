// Exhaustive feature-coverage example for the Companion microSatellite library.
//
// Registers every callback the library offers, then runs through every
// public method on Satellite, SatelliteSurface and SatelliteSubscription
// once the connection is ready. Intended as a reference for what's
// available, not as an interactive tool - watch the serial log to see what
// each call does.

#include <Arduino.h>
#include <WiFi.h>
#include "companion-satellite.hpp"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

// Satellite instance, using a non-default API port to exercise that constructor path.
Satellite satellite("exhaustive-test", 9998);

// Input variable: a value this device produces (device -> Companion).
SatelliteSurfaceVariable inputVariable = {
    .id = "input1",
    .type = SatelliteSurfaceVariableType::VARIABLE_INPUT,
    .name = "Input 1",
    .description = "Test input variable"};

// Output variable: a value Companion produces (Companion -> device).
SatelliteSurfaceVariable outputVariable = {
    .id = "output1",
    .type = SatelliteSurfaceVariableType::VARIABLE_OUTPUT,
    .name = "Output 1",
    .description = "Test output variable"};

// Simple grid surface exercising every option: bitmaps, colors, text, brightness, pincode, config fields, page nav.
SatelliteSurfaceConfig gridConfig = {
    .totalKeys = 8,
    .keysPerRow = 4,
    .sendColors = true,
    .sendText = true,
    .sendTextStyle = true,
    .supportsBrightness = true,
    .pincodeSupport = SatelliteSurfacePincodeSupport::FULL,
    .configFieldsJson = R"([{"id":"demo_field","type":"textinput","label":"Demo Field"}])",
    .canChangePageLabel = "Allow this device to change pages",
    .bitmapSize = 72,
    .bitmapFormat = "png",
};

SatelliteSurface gridSurface(satellite, "Exhaustive Grid", gridConfig, {inputVariable, outputVariable});

// Layout surface: one small-bitmap key, two color-only LED buttons, and an
// encoder with a 24 LED ring. Each control only receives what it asks for.
SatelliteSurfaceConfig layoutConfig = {
    .supportsBrightness = true,
    .layout = SatelliteLayout{
        .stylePresets = {
            {.name = "default", .bitmapWidth = 64, .bitmapHeight = 64, .text = true},
            {.name = "led", .colors = true},
            {.name = "encoder", .colors = true, .ledSegments = 24, .ledsMode = SatelliteLedsMode::FULL_RING},
        },
        .controls = {
            {.id = "0/0", .row = 0, .column = 0},
            {.id = "0/1", .row = 0, .column = 1, .stylePreset = "led"},
            {.id = "0/2", .row = 0, .column = 2, .stylePreset = "led"},
            {.id = "1/0", .row = 1, .column = 0, .stylePreset = "encoder"},
        },
    },
};

SatelliteSurface layoutSurface(satellite, "Exhaustive Layout", layoutConfig);

// Button subscription - an alternative to a full surface, following one
// location. Needs subscriptions enabled in Companion's settings.
SatelliteSubscriptionConfig subscriptionConfig = {
    .bitmapSize = 72,
    .bitmapFormat = "png",
    .colors = true,
    .text = true,
    .textStyle = true,
};

SatelliteSubscription subscription(satellite, "1/0/0", subscriptionConfig);

bool walkthroughDone = false;

static void logState(const char *who, const SatelliteControlState &state)
{
    ESP_LOGI(who, "index=%u controlId=%s type=%d pressed=%d text=%s location=%s bitmap=%u bytes%s leds=%u bytes",
             state.index, state.controlId.c_str(), static_cast<int>(state.type),
             state.pressed.value_or(false),
             state.text.value_or("<none>").c_str(),
             state.location.value_or("<none>").c_str(),
             state.bitmap ? static_cast<unsigned>(state.bitmap->decodedLength()) : 0u,
             state.bitmap && state.bitmap->isDataUrl() ? " (data url)" : "",
             state.leds ? static_cast<unsigned>(state.leds->decodedLength()) : 0u);
}

static void runWalkthrough()
{
    ESP_LOGI(__func__, "deviceName=%s hostname=%s target=%s:%u companionVersion=%s satelliteApiVersion=%s",
             satellite.getDeviceName().c_str(), satellite.getHostname().c_str(),
             satellite.getTargetHostname().c_str(), satellite.getTargetPort(),
             satellite.getCompanionVersion().c_str(), satellite.getSatelliteApiVersion().c_str());

    for (uint8_t i = 0; i < static_cast<uint8_t>(SatelliteFeature::Count); i++)
    {
        SatelliteFeature feature = static_cast<SatelliteFeature>(i);
        ESP_LOGI(__func__, "feature %s: %d", satellite_protocol::featureDefinition(feature).name, satellite.hasFeature(feature));
    }
    for (const String &format : satellite.getCapsBitmapFormats())
    {
        ESP_LOGI(__func__, "bitmap format: %s", format.c_str());
    }

    // Grid surface actions.
    gridSurface.buttonAction(0, SatelliteAction::PRESSED);
    gridSurface.buttonAction(0, SatelliteAction::RELEASED);
    gridSurface.buttonAction(1, SatelliteAction::RIGHT);
    gridSurface.buttonAction(1, SatelliteAction::LEFT);
    gridSurface.rotate(1, 5); // one message with ROTARY_AMOUNT, five without
    gridSurface.rotate(1, -3);

    gridSurface.setVariable("input1", "hello from the walkthrough");

    gridSurface.pressPincodeKey(1);
    gridSurface.pressPincodeKey(2);

    gridSurface.reportFirmwareUpdateInfo("https://example.com/firmware.bin");
    gridSurface.reportFirmwareUpdateInfo(""); // clear it again

    gridSurface.changePage(SatellitePageDirection::NEXT);
    gridSurface.changePage(SatellitePageDirection::PREVIOUS);

    // Layout surface actions, by index (position in the controls list) or by id.
    layoutSurface.buttonAction(1, SatelliteAction::PRESSED);
    layoutSurface.buttonAction(1, SatelliteAction::RELEASED);
    layoutSurface.controlAction("0/2", SatelliteAction::PRESSED);
    layoutSurface.controlAction("0/2", SatelliteAction::RELEASED);
    layoutSurface.rotateControl("1/0", 4);

    // Subscription actions.
    if (subscription.isActive())
    {
        subscription.action(SatelliteAction::PRESSED);
        subscription.action(SatelliteAction::RELEASED);
        subscription.rotate(2);
        subscription.setLocation("1/0/1");
    }
    else
    {
        ESP_LOGW(__func__, "Subscription %s at %s is not active (subscriptions enabled: %d)",
                 subscription.getID().c_str(), subscription.getLocation().c_str(), satellite.supportsSubscriptions());
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
        ESP_LOGI(__func__, "Connecting to WiFi...");
    }
    ESP_LOGI(__func__, "Connected to WiFi");

    // Register every callback the library exposes; each just logs what it received.
    satellite.setOnConnectionStateChange([](SatelliteConnectionState state)
                                         { ESP_LOGI("onConnectionStateChange", "%s (ready=%d connected=%d)",
                                                    satelliteConnectionStateName(state), satellite.isReady(), satellite.isConnected()); });

    gridSurface.setOnKeyStateChange([](const SatelliteControlState &state)
                                    { logState("grid onKeyStateChange", state); });
    gridSurface.setOnKeysClear([]()
                               { ESP_LOGI("onKeysClear", "keys cleared"); });
    gridSurface.setOnBrightnessSet([](uint8_t brightness)
                                   { ESP_LOGI("onBrightnessSet", "brightness=%u", brightness); });
    gridSurface.setOnVariableSet([](const String &id, const String &value)
                                 { ESP_LOGI("onVariableSet", "id=%s value=%s", id.c_str(), value.c_str()); });
    gridSurface.setOnLockedStateSet([](const SatelliteLockedState &state)
                                    { ESP_LOGI("onLockedStateSet", "locked=%d characters=%u rotation=%d", state.locked, state.characterCount, state.rotation); });
    gridSurface.setOnDeviceConfig([](const String &configJson)
                                  { ESP_LOGI("onDeviceConfig", "config=%s", configJson.c_str()); });
    gridSurface.setOnRegistered([]()
                                { ESP_LOGI("onRegistered", "grid surface registered: %d", gridSurface.isRegistered()); });
    gridSurface.setOnError([](const String &command, const String &message)
                           { ESP_LOGW("onError", "%s: %s", command.c_str(), message.c_str()); });

    layoutSurface.setOnKeyStateChange([](const SatelliteControlState &state)
                                      { logState("layout onKeyStateChange", state); });
    layoutSurface.setOnError([](const String &command, const String &message)
                             { ESP_LOGW("layout onError", "%s: %s", command.c_str(), message.c_str()); });

    subscription.setOnStateChange([](const SatelliteControlState &state)
                                  { logState("subscription onStateChange", state); });
    subscription.setOnError([](const String &message)
                            { ESP_LOGW("subscription onError", "%s", message.c_str()); });

    SatelliteOptions options;
    options.dispatch = SatelliteDispatchMode::LOOP;
    options.maxLineLength = 32768;
    satellite.begin(options);
}

void loop()
{
    satellite.loop();

    // Once everything has registered, run through the API once.
    if (!walkthroughDone && satellite.isReady() && gridSurface.isRegistered() && layoutSurface.isRegistered())
    {
        walkthroughDone = true;
        runWalkthrough();
    }
    if (!satellite.isReady())
    {
        walkthroughDone = false;
    }

    delay(10);
}
