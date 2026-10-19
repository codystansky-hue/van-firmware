// VanNet.cpp -- see VanNet.h.
#include "VanNet.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_ota_ops.h>

// The core must not confirm new firmware at boot; trialLoop() does, once a page has answered.
extern "C" bool verifyRollbackLater() { return true; }

namespace vannet {

static const uint32_t AP_FALLBACK_MS = 20000;         // router unreachable this long -> hotspot on
static const uint32_t AP_IDLE_OFF_MS = 60000;         // router up + nobody on the hotspot this long -> hotspot off
static const uint32_t TRIAL_MS = 10UL * 60 * 1000;    // new firmware must answer a web request within this or roll back

static Config C;
static WebServer* srv = nullptr;
static Preferences prefs;
static String staSsid, staPass;       // van router, set on the page ("" = hotspot only)
static String apPass, otaPass;        // set on the setup page

String jsonEsc(const String& s) {
  String o;
  for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; if ((uint8_t)ch >= 0x20) o += ch; }
  return o;
}

static void sendJson(const String& s) { srv->send(200, "application/json", s); }

// ------------------------------------------------------------------ Wi-Fi: van router first, own hotspot as fallback
// Retries are done here, not by the Wi-Fi driver, and wait while a phone is on the hotspot: a join attempt scans every
// channel and would make the hotspot drop out under it.
static bool apOn = false, staUp = false;
static uint32_t staDownMs = 0, staTryMs = 0, apBusyMs = 0;

static void apStart() {
  if (apOn) return;
  WiFi.softAP(C.apSsid, apPass.c_str());
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // SuperMini boards often fail to connect at full power
  apOn = true; apBusyMs = millis();
  Serial.printf("hotspot \"%s\" on: http://%s\n", C.apSsid, WiFi.softAPIP().toString().c_str());
}
static void apStop() {
  if (!apOn) return;
  WiFi.softAPdisconnect(true);
  apOn = false;
  Serial.println("hotspot off (router is up)");
}
static void staJoin() {
  staTryMs = millis();
  WiFi.begin(staSsid.c_str(), staPass.c_str());
}

static void wifiLoop() {
  uint32_t now = millis();
  if (!staSsid.length()) { apStart(); return; }          // no router saved: hotspot only
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != staUp) {
    staUp = up;
    if (up) Serial.printf("joined \"%s\": http://%s.local = http://%s\n", staSsid.c_str(), C.hostname,
                          WiFi.localIP().toString().c_str());
    else { staDownMs = now; Serial.println("router link down"); }
  }
  bool phoneOnAp = apOn && WiFi.softAPgetStationNum() > 0;
  if (phoneOnAp) apBusyMs = now;
  if (up) { if (apOn && now - apBusyMs > AP_IDLE_OFF_MS) apStop(); return; }
  if (!apOn && now - staDownMs > AP_FALLBACK_MS) apStart();
  if (!phoneOnAp && now - staTryMs > (apOn ? 60000UL : 10000UL)) staJoin();
}

static void wifiChanged() {           // router saved or forgotten on the page
  staUp = false; staDownMs = millis();
  if (staSsid.length()) staJoin();    // a hotspot that is on stays on, so the page can show the result
  else { WiFi.disconnect(); apStart(); }
}

bool routerUp() { return staUp; }
bool hotspotOn() { return apOn; }
const String& routerSsid() { return staSsid; }

String statusFields() {
  char buf[160];
  snprintf(buf, sizeof(buf), "\"wifi_up\":%s,\"wifi_ip\":\"%s\",\"wifi_rssi\":%d,\"ap\":%s,\"ap_clients\":%d,\"wifi_ssid\":\"",
           staUp ? "true" : "false", staUp ? WiFi.localIP().toString().c_str() : "", staUp ? WiFi.RSSI() : 0,
           apOn ? "true" : "false", apOn ? WiFi.softAPgetStationNum() : 0);
  return String(buf) + jsonEsc(staSsid) + "\"";
}

static void apiWifi() {               // POST ssid + pass = save and join, forget = hotspot only
  if (srv->hasArg("forget")) { staSsid = ""; staPass = ""; }
  else {
    String ss = srv->arg("ssid"), pw = srv->arg("pass");
    ss.trim();                        // the iPhone keyboard likes to add a trailing space
    if (!ss.length() || ss.length() > 32 || (pw.length() && (pw.length() < 8 || pw.length() > 63))) {
      srv->send(400, "text/plain", "network name 1-32 characters, password 8-63 (empty only for an open network)");
      return;
    }
    staSsid = ss; staPass = pw;
  }
  prefs.putString("ssid", staSsid);
  prefs.putString("wpass", staPass);
  Serial.printf("router: %s\n", staSsid.length() ? staSsid.c_str() : "(forgotten)");
  wifiChanged();
  sendJson("{" + statusFields() + "}");
}

static void apiScan() {               // first call starts a scan, then {"busy":true} until {"nets":[...]}
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) { sendJson("{\"busy\":true}"); return; }
  if (n < 0) { WiFi.scanNetworks(true); sendJson("{\"busy\":true}"); return; }
  String s = "{\"nets\":[";
  int k = 0;
  for (int i = 0; i < n; i++) {
    String e = "\"" + jsonEsc(WiFi.SSID(i)) + "\"";
    if (WiFi.SSID(i).length() == 0 || s.indexOf(e) >= 0) continue;   // hidden, or the same network on another access point
    if (k++) s += ',';
    s += e;
  }
  WiFi.scanDelete();
  sendJson(s + "]}");
}

// ------------------------------------------------------------------ setup login and passwords
bool setupAuth() {
  if (srv->authenticate(C.setupUser, otaPass.c_str())) return true;
  srv->requestAuthentication();
  return false;
}

static bool badPass(const String& p) { return p.length() && (p.length() < 8 || p.length() > 63); }
String passwordError() {
  return (badPass(srv->arg("ap_pass")) || badPass(srv->arg("ota_pass"))) ? "passwords: 8 to 63 characters" : "";
}
bool savePasswords() {
  String ap = srv->arg("ap_pass"), op = srv->arg("ota_pass");
  if (ap.length()) prefs.putString("appass", ap);
  if (op.length()) prefs.putString("otapass", op);
  return ap.length() > 0;
}

void sendRestarting(const String& msg) {
  srv->send(200, "text/html", "<!doctype html><meta name=viewport content='width=device-width'>"
            "<meta http-equiv=refresh content='20;url=/'><body style='font-family:system-ui;font-size:20px;padding:16px'>"
            + msg + " The page comes back in 20 s.");
  delay(500);
}

// ------------------------------------------------------------------ firmware updates
static bool fwTrial = false, webSeen = false;
void pageSeen() { webSeen = true; }

static void trialLoop() {
  if (!fwTrial) return;
  if (webSeen) { esp_ota_mark_app_valid_cancel_rollback(); fwTrial = false; Serial.println("new firmware confirmed"); }
  else if (millis() > TRIAL_MS) {
    Serial.println("new firmware never answered: going back to the previous one");
    delay(100);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

static bool otaAuthOk = false;
static String otaErr;
static void webUpdateUpload() {
  HTTPUpload& up = srv->upload();
  if (up.status == UPLOAD_FILE_START) {
    otaErr = "";
    otaAuthOk = srv->authenticate(C.setupUser, otaPass.c_str());
    if (!otaAuthOk) return;
    C.safeStop("update");
    Serial.printf("web update: %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaErr = Update.errorString();
  } else if (!otaAuthOk || otaErr.length()) {
    if (up.status == UPLOAD_FILE_ABORTED && Update.isRunning()) Update.abort();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (Update.write(up.buf, up.currentSize) != up.currentSize) otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) otaErr = Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    Update.abort(); otaErr = "upload interrupted";
  }
}
static void webUpdateDone() {
  if (!otaAuthOk) return srv->requestAuthentication();
  if (!otaErr.length() && !Update.isFinished()) otaErr = "no complete firmware file received";
  if (otaErr.length()) {
    if (Update.isRunning()) Update.abort();
    Serial.printf("web update failed: %s\n", otaErr.c_str());
    srv->send(500, "text/plain", "Update failed: " + otaErr + "\nThe old firmware is still running.");
    return;
  }
  sendRestarting("Update received, restarting.");
  ESP.restart();
}

// ------------------------------------------------------------------ begin / loop
void begin(WebServer& server, const Config& cfg) {
  C = cfg; srv = &server;
  prefs.begin(C.prefsNs, false);
  staSsid = prefs.getString("ssid", "");
  staPass = prefs.getString("wpass", "");
  apPass  = prefs.getString("appass", C.defaultApPass);
  otaPass = prefs.getString("otapass", C.defaultOtaPass);

  esp_ota_img_states_t ota;
  fwTrial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota) == ESP_OK && ota == ESP_OTA_IMG_PENDING_VERIFY;
  if (fwTrial) Serial.println("new firmware on trial: open the page within 10 min to keep it");

  WiFi.persistent(false);             // the router lives in our settings, not in the Wi-Fi driver's own storage
  WiFi.setHostname(C.hostname);
  WiFi.setAutoReconnect(false);       // wifiLoop() does the retries
  staDownMs = millis();
  if (staSsid.length()) { WiFi.mode(WIFI_STA); WiFi.setTxPower(WIFI_POWER_8_5dBm); staJoin(); }
  else apStart();

  ArduinoOTA.setHostname(C.hostname);
  ArduinoOTA.setPassword(otaPass.c_str());
  ArduinoOTA.onStart([]() { C.safeStop("update"); });
  ArduinoOTA.onEnd([]() { Serial.println("update received, restarting"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("update failed (error %u), old firmware still running\n", (unsigned)e); });
  ArduinoOTA.begin();                 // also starts mDNS: <hostname>.local
  MDNS.addService("http", "tcp", 80);

  server.on("/api/wifi", HTTP_POST, apiWifi);
  server.on("/api/scan", apiScan);
  server.on("/update", HTTP_POST, webUpdateDone, webUpdateUpload);
}

void loop() {
  ArduinoOTA.handle();
  wifiLoop();
  trialLoop();
}

}  // namespace vannet
