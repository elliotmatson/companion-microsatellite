/**
 * @file main.cpp
 * @brief Companion Microsatellite demonstration program
 * @author Elliot Matson
 * @version 1.0
 * @date November 2025
 *
 * This program demonstrates the Companion Satellite API library functionality
 * using an ESP32 microcontroller with NeoPixel LEDs. It creates a virtual
 * control surface with 4 buttons arranged in a 2x2 grid and demonstrates:
 *
 * - WiFi connection and satellite initialization
 * - Surface configuration with variables and callbacks
 * - Button input simulation via GPIO interrupt
 * - NeoPixel output synchronized with button state changes
 * - Brightness control integration
 * - Event handling for satellite connection states
 *
 * Hardware Requirements:
 * - ESP32 microcontroller
 * - 4 NeoPixel LEDs connected to pin 18
 * - Button connected to pin 0 (optional, simulated in loop)
 *
 * The program creates one satellite with one surface containing:
 * - 4 keys total (2 keys per row)
 * - Color and text support enabled
 * - Brightness control enabled
 * - Two variables (one input, one output)
 */

#include <Arduino.h>

#include <Preferences.h>

#include "secrets.h"
#include "companion-satellite.hpp"
#include "WiFi.h"

// NeoPixel library for controlling addressable LEDs
#include "Adafruit_NeoPixel.h"

// Hardware Configuration
/** @brief Pin number for NeoPixel data line */
#define NEOPIXEL_PIN 18
/** @brief Number of NeoPixel LEDs in the strip */
#define NEOPIXEL_COUNT 4

/** @brief NeoPixel strip object for controlling LEDs */
Adafruit_NeoPixel strip(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

/** @brief Main satellite instance for connecting to Companion */
Satellite satellite("microsatellite-testing");
// Satellite satellite2("microsatellite-2nd", 8888); // Example of second satellite

/** @brief Surface configuration structure defining layout and capabilities */
SatelliteSurfaceConfig surfaceConfig = {
    .totalKeys = 4,             // 4 buttons total
    .keysPerRow = 2,            // 2x2 grid layout
    .sendBitmaps = false,       // No bitmap support (saves bandwidth)
    .sendColors = true,         // Enable color control for NeoPixels
    .sendText = true,           // Enable text labels
    .sendTextStyle = true,      // Enable text styling
    .supportsBrightness = true, // Enable brightness control
    // Example config-fields payload for Companion ADD-DEVICE CONFIG_FIELDS
    .configFieldsJson = R"([{"id":"device_label","type":"textinput","label":"Device Label"}])"};

/** @brief Example input variable that can be set from Companion */
SatelliteSurfaceVariable var1 = {
    .id = "var1",
    .type = SatelliteSurfaceVariableType::VARIABLE_INPUT,
    .name = "Variable 1",
    .description = "An input variable"};

/** @brief Example output variable that can be read by Companion */
SatelliteSurfaceVariable var2 = {
    .id = "var2",
    .type = SatelliteSurfaceVariableType::VARIABLE_OUTPUT,
    .name = "Variable 2",
    .description = "An output variable"};

/** @brief Main surface instance with 4 buttons and 2 variables */
SatelliteSurface surface1(satellite, "Surface 1", surfaceConfig, {var1, var2});

/** @brief Task handle for button processing task */
TaskHandle_t buttonTaskHandle = NULL;

/**
 * @brief FreeRTOS task for handling button press events
 * @param parameter Task parameter (unused)
 *
 * This task waits for notifications from the GPIO interrupt and toggles
 * the button state, sending appropriate press/release actions to Companion.
 */
void buttonTask(void *parameter)
{
  static bool buttonState = false;
  for (;;)
  {
    // wait for notify from ISR
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    buttonState = !buttonState;
    surface1.buttonAction(0, buttonState ? SatelliteAction::PRESSED : SatelliteAction::RELEASED);
  }
}

/**
 * @brief Arduino setup function - runs once at startup
 *
 * Initializes all components in the following order:
 * 1. Serial communication for debugging
 * 2. WiFi connection using credentials from secrets.h
 * 3. Satellite initialization and event handling
 * 4. Button task creation and GPIO interrupt setup
 * 5. NeoPixel initialization and callback setup
 *
 * The function sets up callbacks for:
 * - Key state changes: Updates NeoPixel colors based on button states
 * - Brightness changes: Controls NeoPixel brightness from Companion
 */
void setup()
{
  Serial.begin();
  Serial.setDebugOutput(true);

  delay(1000);

  // Connect to Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED)
  {
    delay(1000);
    ESP_LOGI(__func__, "Connecting to WiFi...");
  }
  ESP_LOGI(__func__, "Connected to WiFi");
  satellite.begin();
  ESP_LOGI(__func__, "Companion Satellite Initialized");

  // SatelliteSurface surface2(satellite, "Surface 2", 4, 2); // Optional: Example of multiple surfaces

  // Create FreeRTOS task for handling button events from GPIO interrupt
  xTaskCreate(
      buttonTask,       /* Task function */
      "Button Task",    /* Task name */
      2048,             /* Stack size in bytes */
      NULL,             /* Task parameter */
      1,                /* Task priority */
      &buttonTaskHandle /* Task handle for notifications */
  );

  // Setup GPIO interrupt on pin 0 (BOOT button) to trigger button task
  attachInterrupt(0, [](void)
                  {
                    // Notify button task from ISR context
                    vTaskNotifyGiveFromISR(buttonTaskHandle, NULL); }, CHANGE);

  // Initialize NeoPixel strip
  strip.begin();
  strip.show(); // Turn off all pixels initially

  // Configure callback to update NeoPixel colors when key states change in Companion
  surface1.setOnKeyStateChange([&](const SatelliteSurfaceKeyState &state)
                               {
                                 if (state.index >= NEOPIXEL_COUNT)
                                 {
                                   return;
                                 }

                                 if (state.text.has_value())
                                 {
                                   ESP_LOGI(__func__, "Key %d text: %s", state.index, state.text->c_str());
                                 }

                                 // Update NeoPixel color to match Companion key color when provided.
                                 if (state.keyColor.has_value())
                                 {
                                   strip.setPixelColor(state.index, strip.Color(state.keyColor->r, state.keyColor->g, state.keyColor->b));
                                   strip.show();
                                 } });

  // Configure callback to update NeoPixel brightness when changed in Companion
  surface1.setOnBrightnessSet([&](uint8_t brightness)
                              {
                                // Companion brightness callback is 0-100. NeoPixel expects 0-255.
                                uint8_t mappedBrightness = map(brightness, 0, 100, 0, 255);
                                strip.setBrightness(mappedBrightness);
                                strip.show(); });

  // Example: receive DEVICE-CONFIG updates as decoded JSON.
  surface1.setOnDeviceConfig([&](const String &configJson)
                             { ESP_LOGI(__func__, "DEVICE-CONFIG payload: %s", configJson.c_str()); });

  // Example: receive SUB-STATE updates from ADD-SUB subscriptions.
  satellite.setOnSubscriptionState([&](const Satellite::SubscriptionState &state)
                                   {
                                     ESP_LOGI(__func__, "SUB-STATE for %s", state.subId.c_str());
                                     if (state.text.has_value())
                                     {
                                       ESP_LOGI(__func__, "  text=%s", state.text.value().c_str());
                                     }
                                     if (state.keyColor.has_value())
                                     {
                                       ESP_LOGI(__func__, "  color=#%02x%02x%02x", state.keyColor->r, state.keyColor->g, state.keyColor->b);
                                     } });

  // Example: register a simple-style subscription from setup after protocol readiness.
  const unsigned long readyDeadlineMs = millis() + 10000;
  while (!satellite.isReady() && millis() < readyDeadlineMs)
  {
    delay(50);
  }

  if (!satellite.isReady())
  {
    ESP_LOGW(__func__, "Skipping ADD-SUB example: satellite did not become ready within timeout");
  }
  else if (!satellite.supportsSubscriptions())
  {
    ESP_LOGW(__func__, "Skipping ADD-SUB example: subscriptions not advertised in CAPS");
  }
  else
  {
    satellite.addSubscription(
        "demo-sub-1",
        "1/1/1",
        72,
        String("rgb"),
        true,
        true,
        true);
  }
}

/**
 * @brief Arduino main loop function - runs continuously
 *
 * Currently contains demonstration code that is mostly commented out.
 * The actual button handling is performed by the GPIO interrupt and
 * buttonTask, so this loop just maintains a 1-second delay.
 *
 * In a real application, this loop could be used for:
 * - Reading sensors
 * - Updating variables
 * - Performing periodic tasks
 * - Managing additional I/O operations
 */
void loop()
{
  // Print stats
  ESP_LOGI(__func__, "Free heap: %d", ESP.getFreeHeap());
  ESP_LOGI(__func__, "CPU Frequency: %d MHz", ESP.getCpuFreqMHz());
  ESP_LOGI(__func__, "Task count: %d", uxTaskGetNumberOfTasks());

  // FreeRTOS tasks runtime, stack size, and high water mark, formatted in a table

  char pcWriteBuffer[1024];
  vTaskList(pcWriteBuffer);
  ESP_LOGI(__func__, "Task List:\n%s", pcWriteBuffer);

  delay(10000);
}