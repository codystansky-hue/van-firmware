// renogy.h -- Renogy smart LiFePO4 battery with built-in Bluetooth (RBT200LFP12-BT, advertises as BT-TH-xxxxxxxx).
// Its Bluetooth module passes Modbus requests to the battery's BMS: write to FFD1, the answer comes back on FFF1
// (the protocol used by the cyrils/renogy-bt project). The hub connects, reads, and disconnects again every 30 s, so
// the DC Home app can still connect in between (the module takes one connection at a time).
#pragma once
#include <Arduino.h>
#include <WebServer.h>
#include <NimBLEDevice.h>

namespace renogy {

void begin();                               // settings from flash
void onAdvert(const NimBLEAddress& addr, const char* name, int rssi);   // from the Bluetooth scan (NimBLE task)
bool due();                                 // time to read again
bool busy();                                // connected / reading right now
void poll();                                // connect, read, disconnect: a few seconds, run it in the worker task

String json();                              // {...} for /api/status
String settingsHtml();                      // the Renogy part of the setup form
String statusHtml();                        // one line: last reading, or what went wrong
void saveSettings(WebServer& server);       // from POST /api/hub
void printStatus();                         // serial '?'

}  // namespace renogy
