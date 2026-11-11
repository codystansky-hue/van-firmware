// hub.ino -- the van hub, ESP32-C3 SuperMini (HW-466AB): battery, solar, water and the MaxxFan on one page, http://van.local
// v1 (2026-10-11, compiled, not flashed yet): Victron Instant Readout over Bluetooth (2 SmartSolar + the SmartShunt);
//   Renogy RBT200LFP12-BT battery over Bluetooth (Modbus, read every 30 s); water used from the flow meter that the
//   old Arduino Uno read (450 pulses per litre, 25 gal tank), counted from "tank filled" and kept through power cuts;
//   the MaxxFan's status and controls through its HTTP API. Wi-Fi and updates: libraries/vannet, as the MaxxFan.
// v1.1 (2026-11-10, still not flashed): the MaxxFan card catches up with MaxxFan v4.1 on the FOC board: the motor's rpm,
//   current, watts and supply voltage, faults in plain words with a clear button, the 10 speed steps on a slider, where
//   the knob is (click / vent / up), and a graph of the last 2 or 10 min. The hub fetches the fan's /api/history every
//   30 s (GET /api/maxxfan_history serves that copy); /api/maxxfan also passes on rpm= and clear=1. Older MaxxFan
//   firmware and the aqx fan modes show as before.
//
// Arduino core "esp32" by Espressif 3.3.12 + NimBLE-Arduino 2.5.1. Board settings in sketch.yaml (as the MaxxFan).
// Build: arduino-cli compile --libraries libraries --output-dir build/hub hub. Wiring and use: README.md next to this file.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <VanNet.h>
#include "util.h"
#include "victron.h"
#include "renogy.h"
#include "pages.h"

// ------------------------------------------------------------------ configuration
#define FW_VERSION "v1.1"
const char* FW_BUILD = FW_VERSION " " __DATE__ " " __TIME__;

void safeStop(const char* why);
const vannet::Config NET = {
  .hostname       = "van",            // http://van.local on the van router (mDNS)
  .apSsid         = "VanHub",         // the hub's own hotspot (fallback when the router is unreachable)
  .setupUser      = "van",            // setup page login; its password is also the PC-upload password
  .defaultApPass  = "vanhub123",      // first-boot passwords only: change them on the setup page, which keeps them on
  .defaultOtaPass = "vanhub-ota",     //   the hub (not in git)
  .prefsNs        = "hub",
  .safeStop       = safeStop,
};

// pins (ESP32-C3 GPIO numbers = the "G" labels on the SuperMini)
const int PIN_LED  = 8;               // on-board blue LED, LOW = on
const int PIN_FLOW = 10;              // G10 <- 10k/20k divider <- flow meter signal (5 V sensor), counts rising edges

// ------------------------------------------------------------------ settings ("hub" namespace, with vannet's)
Preferences prefs;
float cfgPpl;                         // flow meter pulses per litre, 0 = none
float cfgTankL;                       // tank size, litres (0 = unknown: show used only)
bool  cfgMetric;                      // page units: false = US (gal, F)
String cfgMaxxfan;                    // "maxxfan" (mDNS name) or an IP

void loadSettings() {
  prefs.begin("hub", false);
  cfgPpl     = prefs.getFloat("ppl", 450.0f);           // the old Arduino: litres = pulses / 7.5 / 60
  cfgTankL   = prefs.getFloat("tank", 94.635f);         // the old Arduino: 25 US gal
  cfgMetric  = prefs.getBool("metric", false);
  cfgMaxxfan = prefs.getString("maxxfan", "maxxfan");
}

volatile bool paused = false;         // an update is running: no Bluetooth, no polling
uint32_t pausedMs = 0;                // a failed update doesn't restart: loop() resumes after 3 min

// ------------------------------------------------------------------ Bluetooth scan (Victron and Renogy advertisements)
class ScanCb : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    std::string name = d->haveName() ? d->getName() : std::string();
    for (uint8_t i = 0; i < d->getManufacturerDataCount(); i++) {
      std::string m = d->getManufacturerData(i);
      if (m.size() >= 4 && (uint8_t)m[0] == 0xE1 && (uint8_t)m[1] == 0x02)
        victron::onAdvert(d->getAddress().toString().c_str(), (const uint8_t*)m.data(), m.size(), name.c_str(), d->getRSSI());
    }
    if (!name.empty()) renogy::onAdvert(d->getAddress(), name.c_str(), d->getRSSI());
  }
} scanCb;

void bleScanStart() {
  if (paused) return;
  NimBLEScan* sc = NimBLEDevice::getScan();
  if (!sc->isScanning()) sc->start(0, false, true);      // 0 = until stopped
}

void bleBegin() {
  NimBLEDevice::init("");
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setScanCallbacks(&scanCb, true);  // every advertisement, not only the first: Victron updates its readings in them
  sc->setActiveScan(true);              // scan responses carry the names (BT-TH-..., the Victron custom names)
  sc->setInterval(100);                 // ms: listen 50 of every 100 ms, the rest is left to Wi-Fi (one radio)
  sc->setWindow(50);
  sc->setMaxResults(0);                 // callbacks only, keep nothing
  bleScanStart();
}

// ------------------------------------------------------------------ water: flow meter pulses -> litres used since the fill
// Counted in RAM, saved when the water stops (5 s without pulses), so a power cut loses at most the running draw.
volatile uint32_t flowPulses = 0, flowLastUs = 0;
void IRAM_ATTR onFlow() {             // the sensor gives ~225 Hz at 30 L/min: edges closer than 1 ms are noise
  uint32_t t = micros();
  if (t - flowLastUs < 1000) return;
  flowLastUs = t;
  flowPulses = flowPulses + 1;        // single writer (this ISR); C++20 deprecates ++ on volatile
}
float waterUsedL = 0, waterSavedL = 0, waterLpm = 0;
uint32_t flowSeen = 0, flowT = 0, flowLastMs = 0;

void waterSave() {
  if (waterUsedL == waterSavedL) return;
  prefs.putFloat("used", waterUsedL);
  waterSavedL = waterUsedL;
}

void waterLoop() {
  uint32_t now = millis();
  if (now - flowT < 1000) return;
  noInterrupts(); uint32_t p = flowPulses; interrupts();
  uint32_t dp = p - flowSeen;
  float dt = (now - flowT) / 60000.0f;
  flowSeen = p; flowT = now;
  if (cfgPpl <= 0) { waterLpm = 0; return; }
  float dl = dp / cfgPpl;
  waterUsedL += dl;
  waterLpm = dl / dt;
  if (dp) flowLastMs = now;
  else if (now - flowLastMs > 5000) waterSave();
}

String waterJson() {
  return "{\"on\":" + String(cfgPpl > 0 ? "true" : "false") + ",\"used\":" + jnum(waterUsedL, 2) + ",\"tank\":" + jnum(cfgTankL, 2)
         + ",\"lpm\":" + jnum(waterLpm, 2) + ",\"pulses\":" + flowSeen + "}";
}

// ------------------------------------------------------------------ MaxxFan: its HTTP API (../maxxfan/README.md, section 8)
SemaphoreHandle_t mfLock;
IPAddress mfIp;                       // resolved address, 0.0.0.0 = look it up again
String mfStatus, mfErr = "not asked yet";
uint32_t mfMs = 0, mfPollMs = 0;

IPAddress mfAddress() {               // the IP, from the setting or mDNS; 0.0.0.0 if not found
  IPAddress ip;
  xSemaphoreTake(mfLock, portMAX_DELAY); ip = mfIp; String host = cfgMaxxfan; xSemaphoreGive(mfLock);
  if (ip != IPAddress()) return ip;
  if (!ip.fromString(host)) {
    if (host.endsWith(".local")) host.remove(host.length() - 6);
    ip = MDNS.queryHost(host, 2000);
  }
  xSemaphoreTake(mfLock, portMAX_DELAY); mfIp = ip; xSemaphoreGive(mfLock);
  return ip;
}

int mfGet(const String& path, String& body) {   // a plain HTTP/1.0 GET; the HTTP status, or -1
  IPAddress ip = mfAddress();
  if (ip == IPAddress()) return -1;
  WiFiClient c;
  if (!c.connect(ip, 80, 1500)) {
    xSemaphoreTake(mfLock, portMAX_DELAY); mfIp = IPAddress(); xSemaphoreGive(mfLock);   // look it up again next time
    return -1;
  }
  c.print("GET " + path + " HTTP/1.0\r\nHost: maxxfan.local\r\nConnection: close\r\n\r\n");
  String r;
  r.reserve(1500);
  char buf[256];
  for (uint32_t t0 = millis(); (c.connected() || c.available()) && millis() - t0 < 2500;) {
    int n = c.read((uint8_t*)buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; r += buf; } else delay(5);
    if (r.length() > 12288) break;    // the fan's /api/history is ~6 KB; its status ~1 KB
  }
  c.stop();
  int h = r.indexOf("\r\n\r\n");
  body = h >= 0 ? r.substring(h + 4) : "";
  return r.startsWith("HTTP/1.") ? r.substring(9, 12).toInt() : -1;
}

void mfStore(int code, const String& body) {
  xSemaphoreTake(mfLock, portMAX_DELAY);
  if (code == 200 && body.startsWith("{")) { mfStatus = body; mfMs = millis(); mfErr = ""; }
  else mfErr = code < 0 ? "MaxxFan not reachable (is it on the router?)" : "MaxxFan answered " + String(code);
  xSemaphoreGive(mfLock);
}

void mfPoll() {
  if (!vannet::routerUp()) {
    xSemaphoreTake(mfLock, portMAX_DELAY); mfErr = "the hub isn't on the router, so it can't reach the MaxxFan"; xSemaphoreGive(mfLock);
    return;
  }
  String body;
  int code = mfGet("/api/status", body);
  mfStore(code, body);
}

// The fan's motor history (MaxxFan v4.1 on the FOC board): fetched every 30 s by the worker, served to the hub page as is,
// with its "age" brought up to date. The page adds the 3 s status readings itself.
String mfHist;
uint32_t mfHistMs = 0, mfHistPollMs = 0;

void mfPollHistory() {
  if (!vannet::routerUp()) return;
  xSemaphoreTake(mfLock, portMAX_DELAY); bool foc = mfStatus.indexOf("\"foc\":") >= 0; xSemaphoreGive(mfLock);
  if (!foc) return;                   // older firmware or an aqx fan mode: no history to get
  String body;
  int code = mfGet("/api/history", body);
  if (code != 200 || !body.startsWith("{\"dt\"")) return;
  xSemaphoreTake(mfLock, portMAX_DELAY); mfHist = body; mfHistMs = millis(); xSemaphoreGive(mfLock);
}

String maxxfanJson() {
  xSemaphoreTake(mfLock, portMAX_DELAY);
  uint32_t age = (millis() - mfMs) / 1000;
  bool ok = mfMs && age < 30;
  String s = ok ? "{\"ok\":true,\"age\":" + String(age) + ",\"s\":" + mfStatus + "}"
                : "{\"ok\":false,\"err\":\"" + vannet::jsonEsc(mfErr) + "\"}";
  xSemaphoreGive(mfLock);
  return s;
}

// ------------------------------------------------------------------ worker task: the slow, blocking jobs
// Renogy (connect + read, a few seconds) and the MaxxFan status (up to 2.5 s), so the pages never wait for them.
void worker(void*) {
  for (;;) {
    if (!paused) {
      if (renogy::due()) renogy::poll();
      if (millis() - mfPollMs >= 3000) { mfPollMs = millis(); mfPoll(); }
      if (millis() - mfHistPollMs >= (mfHistMs ? 30000UL : 6000UL)) { mfHistPollMs = millis(); mfPollHistory(); }
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

void safeStop(const char* why) {      // before an update or a restart: Bluetooth off, water count saved
  paused = true; pausedMs = millis();
  NimBLEDevice::getScan()->stop();
  waterSave();
  Serial.printf("%s: Bluetooth stopped, water count saved\n", why);
}

// ------------------------------------------------------------------ web
WebServer server(80);

void apiStatus() {
  vannet::pageSeen();
  String s = "{\"fw\":\"" + String(FW_BUILD) + "\",\"up\":" + String(millis() / 1000) + ",\"units\":\""
           + (cfgMetric ? "metric" : "us") + "\"," + vannet::statusFields()
           + ",\"victron\":" + victron::json() + ",\"renogy\":" + renogy::json()
           + ",\"water\":" + waterJson() + ",\"maxxfan\":" + maxxfanJson() + "}";
  server.send(200, "application/json", s);
}

// ?lid=open|close|stop, ?speed=0..100&dir=fwd|rev, ?rpm=200..1500&dir=fwd|rev (FOC board), ?clear=1 (the motor chip's
// faults): passed on to the MaxxFan
void apiMaxxfan() {
  String path, dir = server.arg("dir") == "rev" ? "rev" : "fwd";
  if (server.hasArg("lid")) {
    String c = server.arg("lid");
    if (c != "open" && c != "close" && c != "stop") { server.send(400, "text/plain", "lid = open | close | stop"); return; }
    path = "/api/lid?cmd=" + c;
  } else if (server.hasArg("rpm")) {
    path = "/api/fan?rpm=" + String(constrain(server.arg("rpm").toInt(), 0, 1500)) + "&dir=" + dir;
  } else if (server.hasArg("speed")) {
    path = "/api/fan?speed=" + String(constrain(server.arg("speed").toInt(), 0, 100)) + "&dir=" + dir;
  } else if (server.arg("clear") == "1") {
    path = "/api/foc?cmd=clear";
  } else { server.send(400, "text/plain", "lid=..., speed=...&dir=..., rpm=...&dir=... or clear=1"); return; }
  String body;
  int code = mfGet(path, body);
  mfStore(code, body);
  if (code == 200) server.send(200, "application/json", body);
  else server.send(502, "text/plain", code < 0 ? "MaxxFan not reachable" : "MaxxFan answered " + String(code));
}

void apiMaxxfanHistory() {
  xSemaphoreTake(mfLock, portMAX_DELAY);
  String h = mfHist;
  uint32_t since = millis() - mfHistMs;
  xSemaphoreGive(mfLock);
  int a = h.indexOf("\"age\":");
  int e = a < 0 ? -1 : h.indexOf(',', a);
  if (!h.length() || since > 120000UL || e < 0) { server.send(404, "text/plain", "no MaxxFan history yet"); return; }
  float age = h.substring(a + 6, e).toFloat() + since / 1000.0f;
  server.send(200, "application/json", h.substring(0, a + 6) + String(age, 1) + h.substring(e));
}

void apiWater() {                     // POST filled=1: the tank is full, start counting again
  if (server.arg("filled") == "1") {
    waterUsedL = 0; waterSave();
    Serial.println("water: tank filled");
  }
  server.send(200, "application/json", waterJson());
}

String setupMsg;                      // shown once on the next setup page (an error from a form)
void redirectSetup(const String& msg) {
  setupMsg = msg;
  server.sendHeader("Location", "/setup");
  server.send(303);
}

void setupPage() {
  if (!vannet::setupAuth()) return;
  vannet::pageSeen();
  String p(SETUP_PAGE);
  p.replace("%FW%", FW_BUILD);
  p.replace("%MSG%", setupMsg.length() ? "<p class=bad>" + htmlEsc(setupMsg) + "</p>" : String());
  setupMsg = "";
  p.replace("%VICTRON%", victron::setupHtml());
  p.replace("%RENOGY%", renogy::settingsHtml());
  p.replace("%RSTATUS%", renogy::statusHtml());
  p.replace("%PPL%", String(cfgPpl, 1));
  p.replace("%TANK%", String(cfgMetric ? cfgTankL : cfgTankL / 3.78541f, 1));
  p.replace("%TUNIT%", cfgMetric ? "L" : "gal");
  p.replace("%US%", cfgMetric ? "" : "selected");
  p.replace("%METRIC%", cfgMetric ? "selected" : "");
  p.replace("%MF%", htmlEsc(cfgMaxxfan));
  server.send(200, "text/html", p);
}

void apiVictron() {                   // POST mac, name, key (empty = keep), forget
  if (!vannet::setupAuth()) return;
  String name = server.arg("name"); name.trim();
  redirectSetup(victron::save(server.arg("mac"), name, server.arg("key"), server.arg("forget") == "1"));
}

void apiHub() {                       // POST the Renogy, water and MaxxFan settings
  if (!vannet::setupAuth()) return;
  renogy::saveSettings(server);
  bool wasMetric = cfgMetric;
  cfgMetric = server.arg("units") == "metric";
  cfgPpl = constrain(server.arg("ppl").toFloat(), 0.0f, 10000.0f);
  float t = constrain(server.arg("tank").toFloat(), 0.0f, 2000.0f);
  cfgTankL = wasMetric ? t : t * 3.78541f;           // the form showed the tank in the old units
  String mf = server.arg("mf"); mf.trim();
  xSemaphoreTake(mfLock, portMAX_DELAY);
  if (mf != cfgMaxxfan) { cfgMaxxfan = mf.length() ? mf : String("maxxfan"); mfIp = IPAddress(); }
  xSemaphoreGive(mfLock);
  prefs.putFloat("ppl", cfgPpl);
  prefs.putFloat("tank", cfgTankL);
  prefs.putBool("metric", cfgMetric);
  prefs.putString("maxxfan", cfgMaxxfan);
  redirectSetup("");
}

void apiSetup() {                     // POST ap_pass, ota_pass (empty = keep); then restart
  if (!vannet::setupAuth()) return;
  String err = vannet::passwordError();
  if (err.length()) { server.send(400, "text/plain", err); return; }
  bool apChanged = vannet::savePasswords();
  vannet::sendRestarting(String("Saved, restarting.") + (apChanged ? " The VanHub hotspot now uses the new password." : ""));
  safeStop("setup");
  ESP.restart();
}

// ------------------------------------------------------------------ serial: ? = status
void serialLoop() {
  while (Serial.available()) {
    if (Serial.read() != '?') continue;
    Serial.printf("hub %s | router \"%s\" %s %s | hotspot %s | heap %u\n", FW_BUILD, vannet::routerSsid().c_str(),
                  vannet::routerUp() ? "up" : "down", vannet::routerUp() ? WiFi.localIP().toString().c_str() : "",
                  vannet::hotspotOn() ? "on 192.168.4.1" : "off", (unsigned)ESP.getFreeHeap());
    victron::printStatus();
    renogy::printStatus();
    Serial.printf("  water: %.2f L used, %.2f L/min, %lu pulses since boot, %.1f pulses/L\n", waterUsedL, waterLpm,
                  (unsigned long)flowSeen, cfgPpl);
    Serial.printf("  maxxfan: %s\n", maxxfanJson().c_str());
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_FLOW, INPUT_PULLUP);    // 5 V sensor output: through the divider. Bare open collector: no 20k, this pull-up does it
  attachInterrupt(digitalPinToInterrupt(PIN_FLOW), onFlow, RISING);
  Serial.begin(115200);
  delay(300);
  loadSettings();
  waterUsedL = waterSavedL = prefs.getFloat("used", 0);
  flowT = millis();
  mfLock = xSemaphoreCreateMutex();
  victron::begin();
  renogy::begin();

  vannet::begin(server, NET);         // Wi-Fi, mDNS (van.local), updates; adds /api/wifi, /api/scan, POST /update
  server.on("/", []() { vannet::pageSeen(); server.send_P(200, "text/html", PAGE); });
  server.on("/api/status", apiStatus);
  server.on("/api/maxxfan", apiMaxxfan);
  server.on("/api/maxxfan_history", apiMaxxfanHistory);
  server.on("/api/water", HTTP_POST, apiWater);
  server.on("/api/victron", HTTP_POST, apiVictron);
  server.on("/api/hub", HTTP_POST, apiHub);
  server.on("/api/setup", HTTP_POST, apiSetup);
  server.on("/setup", HTTP_GET, setupPage);
  server.on("/update", HTTP_GET, setupPage);
  server.begin();

  bleBegin();
  xTaskCreate(worker, "worker", 8192, nullptr, 1, nullptr);
  Serial.printf("van hub %s ready, router \"%s\"\n", FW_BUILD,
                vannet::routerSsid().length() ? vannet::routerSsid().c_str() : "none: hotspot only");
}

uint32_t lastBlink = 0, lastScanCheck = 0;
void loop() {
  server.handleClient();
  vannet::loop();
  waterLoop();
  serialLoop();
  uint32_t now = millis();
  if (paused && now - pausedMs > 180000) { paused = false; Serial.println("update didn't finish: Bluetooth back on"); }
  if (now - lastScanCheck > 5000) {   // keep the Bluetooth scan running (Renogy reads stop it for a moment)
    lastScanCheck = now;
    if (!renogy::busy()) bleScanStart();
  }
  // LED: short blink every 2 s
  digitalWrite(PIN_LED, (now - lastBlink) % 2000 < 60 ? LOW : HIGH);
  if (now - lastBlink > 2000) lastBlink = now;
  delay(2);
}
