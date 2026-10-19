// victron.h -- Victron "Instant Readout": the encrypted readings Victron Smart devices put in their Bluetooth
// advertisements (the same numbers VictronConnect shows in its device list). Nothing to connect to, no wiring.
// Decoded: solar chargers (SmartSolar MPPT) and battery monitors (SmartShunt, BMV-712). Each device has its own
// encryption key, pasted on the setup page and kept on the ESP.
#pragma once
#include <Arduino.h>

namespace victron {

void begin();                               // keys and names from flash
// from the Bluetooth scan (NimBLE task): mfr = manufacturer data including the company id (E1 02 for Victron)
void onAdvert(const char* mac, const uint8_t* mfr, size_t len, const char* name, int rssi);

String json();                              // [ {...}, ... ] for /api/status
String setupHtml();                         // the device list with the key fields, for /setup
String save(const String& mac, const String& name, const String& keyHex, bool forget);   // "" or an error
void printStatus();                         // serial '?'

}  // namespace victron
