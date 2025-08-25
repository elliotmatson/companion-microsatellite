#include <Arduino.h>

#include "USB.h"
#include "USBMSC.h"
USBMSC MSC; // Resolves a bug in FirmwareMSC that causes the MSC device to load as Read-Only

#include <Preferences.h>

#include "config.h"

Preferences preferences;

void setup()
{
  Serial.begin(115200);
  Serial.setDebugOutput(true); // Enable debug output to USB CDC
  while (!Serial)
  {
    delay(10); // Wait for Serial to be ready
  }
}

void loop()
{
}