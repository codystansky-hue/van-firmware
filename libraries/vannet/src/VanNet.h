// VanNet.h -- Wi-Fi and firmware updates shared by the van's ESP32 sketches (maxxfan, hub).
//
// Wi-Fi: joins the van router (Starlink) saved on the device's page, so the phone stays on the router and opens
// http://<hostname>.local. Router unreachable for 20 s -> the device's own hotspot comes on (http://192.168.4.1); it goes
// off again once the router is back and nobody has been on the hotspot for a minute.
// Updates: a .bin uploaded on the setup page, or a PC on the same network (espota, port 3232), both with the setup
// password. New firmware runs on trial: it must answer a web request (call pageSeen()) within 10 min, or the ESP goes back
// to the previous firmware. Passwords: set on the setup page, kept on the ESP (not in git), survive updates.
//
// Moved out of maxxfan.ino v3 (2026-10-11) so both sketches use one copy.
#pragma once
#include <Arduino.h>
#include <WebServer.h>

namespace vannet {

struct Config {
  const char* hostname;               // http://<hostname>.local on the router (mDNS); also the PC-upload name
  const char* apSsid;                 // the device's own hotspot (fallback)
  const char* setupUser;              // setup page login; its password is also the PC-upload password
  const char* defaultApPass;          // first boot only: changed on the setup page
  const char* defaultOtaPass;
  const char* prefsNs;                // Preferences namespace holding ssid, wpass, appass, otapass
  void (*safeStop)(const char* why);  // stop every output before an update or a restart
};

// In setup(), after Serial.begin(). Starts Wi-Fi, mDNS and ArduinoOTA, and adds POST /api/wifi, GET /api/scan and
// POST /update to the server (the sketch adds its own pages, GET /update included, and calls server.begin()).
void begin(WebServer& server, const Config& cfg);
void loop();                          // every loop(): Wi-Fi, PC uploads, firmware trial

void pageSeen();                      // a page or the status API answered: the new firmware is kept
bool setupAuth();                     // setup page login; on failure it has already asked the browser to log in

String statusFields();                // "wifi_up":..,"wifi_ip":"..","wifi_rssi":..,"ap":..,"ap_clients":..,"wifi_ssid":".."
bool routerUp();
bool hotspotOn();
const String& routerSsid();

String passwordError();               // the form's ap_pass / ota_pass: "" if each is empty or 8-63 characters
bool savePasswords();                 // stores the non-empty ones (used after a restart); true if the hotspot one changed
void sendRestarting(const String& msg);   // "restarting" page that returns to / after 20 s; the caller restarts

String jsonEsc(const String& s);      // for a value inside "..." in JSON

}  // namespace vannet
