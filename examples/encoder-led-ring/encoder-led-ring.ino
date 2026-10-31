// Companion Microsatellite - encoder with an LED ring, using a layout manifest.
//
// Hardware:
//  - a rotary encoder (A/B on pins 32/33, push switch on pin 25)
//  - a ring of 24 WS2812/NeoPixel LEDs around it (data on pin 18)
//  - a single NeoPixel "status" button LED (data on pin 19) plus a push
//    button (pin 26)
//
// Instead of a grid of identical keys, the surface describes each control
// with a layout manifest, so each one only receives what it can show:
//
//  - the encoder asks for its LED ring colors (LEDS, Companion 4.3+ /
//    Satellite API 1.13): set a button's "LED ring" style in Companion (eg.
//    a gauge bound to a volume variable) and the ring follows it
//  - the button only asks for its color - no bitmaps or text, so there is
//    very little data to receive and nothing big to buffer
//
// Encoder turns are counted in an interrupt and reported from loop() as a
// single rotation of N steps (ROTARY_AMOUNT, Satellite API 1.14), so a fast
// spin is one message rather than dozens. With older Companion versions the
// library falls back to one message per step.
//
// Library dependencies (install via Arduino Library Manager):
// - ESPAsyncWebServer (ESP32Async)
// - AsyncTCP (ESP32Async)
// - ArduinoJson
// - Adafruit NeoPixel

#include <Arduino.h>
#include <WiFi.h>
#include <Adafruit_NeoPixel.h>
#include "companion-satellite.hpp"

// WiFi configuration - replace with your own network before flashing, and
// avoid committing real credentials to a tracked file.
#define WIFI_SSID "YOUR_WIFI_SSID"
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"

#define ENCODER_A_PIN 32
#define ENCODER_B_PIN 33
#define ENCODER_SWITCH_PIN 25
#define BUTTON_PIN 26
#define RING_PIN 18
#define RING_LEDS 24
#define BUTTON_LED_PIN 19

Adafruit_NeoPixel ring(RING_LEDS, RING_PIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel buttonLed(1, BUTTON_LED_PIN, NEO_GRB + NEO_KHZ800);

Satellite satellite("microsatellite-encoder");

// Control indexes, matching the order of layout.controls below.
constexpr uint16_t kEncoder = 0;
constexpr uint16_t kButton = 1;

SatelliteSurfaceConfig surfaceConfig = {
    .supportsBrightness = true,
    .layout = SatelliteLayout{
        .stylePresets = {
            // Every layout needs a "default" preset - used by controls that don't name one.
            {.name = "default", .colors = true},
            {.name = "encoder", .colors = true, .ledSegments = RING_LEDS, .ledsMode = SatelliteLedsMode::FULL_RING},
        },
        .controls = {
            {.id = "0/0", .row = 0, .column = 0, .stylePreset = "encoder"},
            {.id = "0/1", .row = 0, .column = 1},
        },
    },
};

SatelliteSurface surface(satellite, "Encoder + LED ring", surfaceConfig);

// Quadrature decoding in an interrupt, accumulating steps until loop() sends them.
volatile int32_t encoderSteps = 0;
volatile uint8_t encoderState = 0;

void IRAM_ATTR onEncoderChange()
{
  // Valid transitions of the (previous AB, current AB) pair: +1 / -1, else 0
  static const int8_t kTransitions[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};
  static int8_t subSteps = 0;

  encoderState = ((encoderState << 2) | (digitalRead(ENCODER_A_PIN) << 1) | digitalRead(ENCODER_B_PIN)) & 0x0f;
  subSteps += kTransitions[encoderState];
  // Most encoders have 4 transitions per detent
  if (subSteps >= 4 || subSteps <= -4)
  {
    encoderSteps += subSteps / 4;
    subSteps = 0;
  }
}

bool lastEncoderPressed = false;
bool lastButtonPressed = false;

void setup()
{
  Serial.begin(115200);
  delay(1000);

  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);
  pinMode(ENCODER_SWITCH_PIN, INPUT_PULLUP);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  attachInterrupt(ENCODER_A_PIN, onEncoderChange, CHANGE);
  attachInterrupt(ENCODER_B_PIN, onEncoderChange, CHANGE);

  ring.begin();
  ring.show();
  buttonLed.begin();
  buttonLed.show();

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

  // Runs in satellite.loop(), so it's safe to drive the LEDs from here.
  surface.setOnKeyStateChange([](const SatelliteControlState &state)
                              {
                                if (state.index == kEncoder && state.leds)
                                {
                                  // One R,G,B triple per segment. Segment 0 is at 6 o'clock,
                                  // increasing clockwise - remap here if your ring is wired
                                  // differently.
                                  uint8_t rgb[RING_LEDS * 3];
                                  size_t length = state.leds->decode(rgb, sizeof(rgb));
                                  for (size_t i = 0; i < length / 3; i++)
                                  {
                                    ring.setPixelColor(i, rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]);
                                  }
                                  ring.show();
                                }
                                else if (state.index == kButton && state.keyColor)
                                {
                                  buttonLed.setPixelColor(0, state.keyColor->r, state.keyColor->g, state.keyColor->b);
                                  buttonLed.show();
                                } });

  surface.setOnBrightnessSet([](uint8_t brightness)
                             {
                               uint8_t level = map(brightness, 0, 100, 0, 255);
                               ring.setBrightness(level);
                               buttonLed.setBrightness(level);
                               ring.show();
                               buttonLed.show(); });

  surface.setOnKeysClear([]()
                         {
                           ring.clear();
                           ring.show();
                           buttonLed.clear();
                           buttonLed.show(); });

  surface.setOnError([](const String &command, const String &message)
                     { Serial.printf("Companion rejected %s: %s\n", command.c_str(), message.c_str()); });

  satellite.begin();
}

void loop()
{
  satellite.loop();

  // Send whatever the encoder moved since last time as one rotation.
  noInterrupts();
  int32_t steps = encoderSteps;
  encoderSteps = 0;
  interrupts();
  if (steps != 0)
  {
    surface.rotate(kEncoder, steps);
  }

  bool encoderPressed = digitalRead(ENCODER_SWITCH_PIN) == LOW;
  if (encoderPressed != lastEncoderPressed)
  {
    lastEncoderPressed = encoderPressed;
    surface.buttonAction(kEncoder, encoderPressed ? SatelliteAction::PRESSED : SatelliteAction::RELEASED);
  }

  bool buttonPressed = digitalRead(BUTTON_PIN) == LOW;
  if (buttonPressed != lastButtonPressed)
  {
    lastButtonPressed = buttonPressed;
    surface.buttonAction(kButton, buttonPressed ? SatelliteAction::PRESSED : SatelliteAction::RELEASED);
  }

  delay(10);
}
