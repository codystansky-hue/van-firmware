// renogy.cpp -- see renogy.h.
//
// Modbus RTU over BLE: request [addr, 3, reg hi, reg lo, count hi, count lo, crc lo, crc hi]; answer
// [addr, 3, bytes, data (big-endian words)..., crc lo, crc hi], possibly split over several notifications. An error answer
// has 0x83 as its second byte. Registers (renogy-bt BatteryClient):
//   5000: cell count, then 16 cell voltages (0.1 V)       5017: sensor count, then 16 temperatures (0.1 C, signed)
//   5042: current (0.01 A, signed, + = charging), voltage (0.1 V), remaining (0.001 Ah, 32 bit), capacity (0.001 Ah, 32 bit)
//   5122: model, 16 characters
#include "renogy.h"
#include "util.h"
#include <Preferences.h>
#include <VanNet.h>

void bleScanStart();                          // hub.ino: the scan is stopped while connected

namespace renogy {

static Preferences prefs;                     // "renogy": name, id, every, on
static String cfgName;                        // Bluetooth name to look for
static uint8_t cfgId = 255;                   // Modbus address: 255 for the battery's own Bluetooth (48 behind a hub)
static uint32_t cfgEveryS = 30;
static bool cfgOn = true;

struct State {
  bool found; NimBLEAddress addr; int rssi; uint32_t seenMs;
  uint32_t dataMs;                            // last good reading, 0 = none
  float v, a, remAh, capAh;
  int cells; float cellV[16];
  int temps; float tempC[16];
  char model[17];
  char err[80];
  uint32_t reads, fails;
};
static State st;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool busyNow = false;
static uint32_t lastPollMs = 0;
static bool polledOnce = false;

static void setErr(const char* e) {
  portENTER_CRITICAL(&mux); strlcpy(st.err, e, sizeof(st.err)); portEXIT_CRITICAL(&mux);
  if (*e) Serial.printf("renogy: %s\n", e);
}

void begin() {
  prefs.begin("renogy", false);
  cfgName  = prefs.getString("name", "BT-TH-66FE625A");   // Cody's RBT200LFP12-BT
  cfgId    = prefs.getUChar("id", 255);
  cfgEveryS = prefs.getUInt("every", 30);
  cfgOn    = prefs.getBool("on", true);
  strlcpy(st.err, "looking for it", sizeof(st.err));
}

void onAdvert(const NimBLEAddress& addr, const char* name, int rssi) {
  if (!cfgName.length() || strcasecmp(name, cfgName.c_str())) return;
  portENTER_CRITICAL(&mux);
  st.found = true; st.addr = addr; st.rssi = rssi; st.seenMs = millis();
  portEXIT_CRITICAL(&mux);
}

bool busy() { return busyNow; }
bool due() { return cfgOn && (!polledOnce || millis() - lastPollMs >= cfgEveryS * 1000UL); }

// ------------------------------------------------------------------ Modbus over BLE
static portMUX_TYPE rxMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t rx[96];
static size_t rxLen = 0;

static void onNotify(NimBLERemoteCharacteristic*, uint8_t* d, size_t n, bool) {
  portENTER_CRITICAL(&rxMux);
  for (size_t i = 0; i < n && rxLen < sizeof(rx); i++) rx[rxLen++] = d[i];
  portEXIT_CRITICAL(&rxMux);
}

static uint16_t crc16(const uint8_t* p, size_t n) {   // Modbus: init FFFF, poly A001 (reflected)
  uint16_t c = 0xFFFF;
  for (size_t i = 0; i < n; i++) { c ^= p[i]; for (int b = 0; b < 8; b++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1; }
  return c;
}

// reads `words` registers from `reg` into out (big-endian bytes); "" or an error
static String readRegs(NimBLERemoteCharacteristic* w, uint16_t reg, uint16_t words, uint8_t* out) {
  uint8_t q[8] = {cfgId, 3, (uint8_t)(reg >> 8), (uint8_t)reg, (uint8_t)(words >> 8), (uint8_t)words, 0, 0};
  uint16_t c = crc16(q, 6);
  q[6] = c & 0xFF; q[7] = c >> 8;
  portENTER_CRITICAL(&rxMux); rxLen = 0; portEXIT_CRITICAL(&rxMux);
  if (!w->writeValue(q, sizeof(q), !w->canWriteNoResponse())) return "write failed";
  size_t need = 5 + 2 * words;
  uint8_t buf[sizeof(rx)];
  for (uint32_t t0 = millis(); millis() - t0 < 3000;) {
    vTaskDelay(pdMS_TO_TICKS(20));
    portENTER_CRITICAL(&rxMux);
    size_t n = rxLen;
    memcpy(buf, rx, n);
    portEXIT_CRITICAL(&rxMux);
    if (n >= 3 && buf[1] == 0x83) return "the battery answered error " + String(buf[2]) + " (wrong Modbus address?)";
    if (n < need) continue;
    if (buf[1] != 3 || buf[2] != 2 * words) return "unexpected answer";
    c = crc16(buf, need - 2);
    if (buf[need - 2] != (c & 0xFF) || buf[need - 1] != (c >> 8)) return "bad checksum";
    memcpy(out, buf + 3, 2 * words);
    return "";
  }
  return "no answer from register " + String(reg) + " (wrong Modbus address?)";
}

static NimBLERemoteCharacteristic* findChar(NimBLEClient* cl, uint16_t uuid) {
  for (auto* s : cl->getServices(true)) {
    auto* ch = s->getCharacteristic(NimBLEUUID(uuid));
    if (ch) return ch;
  }
  return nullptr;
}

static int16_t be16(const uint8_t* p) { return (int16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t* p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | (p[2] << 8) | p[3]; }

void poll() {
  lastPollMs = millis(); polledOnce = true;
  State s;
  portENTER_CRITICAL(&mux); s = st; portEXIT_CRITICAL(&mux);
  if (!s.found || millis() - s.seenMs > 120000) {
    setErr(s.found ? "not heard for 2 min: out of range or asleep" : ("not heard yet (looking for " + cfgName + ")").c_str());
    return;
  }

  static NimBLEClient* cl = nullptr;
  if (!cl) { cl = NimBLEDevice::createClient(); cl->setConnectTimeout(5000); }
  busyNow = true;
  NimBLEDevice::getScan()->stop();
  String err;
  uint8_t info[12], cells[34], temps[34], model[16];
  bool gotCells = false, gotTemps = false, gotModel = false;
  if (!cl->connect(s.addr)) err = "couldn't connect (is the DC Home app connected to it?)";
  else {
    NimBLERemoteCharacteristic* w = findChar(cl, 0xFFD1);
    NimBLERemoteCharacteristic* n = findChar(cl, 0xFFF1);
    if (!w || !n) err = "no FFD1 / FFF1: not a Renogy Bluetooth module?";
    else if (!n->subscribe(true, onNotify)) err = "couldn't subscribe to FFF1";
    else {
      err = readRegs(w, 5042, 6, info);
      if (!err.length()) {
        vTaskDelay(pdMS_TO_TICKS(200));
        gotCells = readRegs(w, 5000, 17, cells) == "";
        vTaskDelay(pdMS_TO_TICKS(200));
        gotTemps = readRegs(w, 5017, 17, temps) == "";
        if (!s.model[0]) { vTaskDelay(pdMS_TO_TICKS(200)); gotModel = readRegs(w, 5122, 8, model) == ""; }
      }
    }
    cl->disconnect();
  }
  busyNow = false;
  bleScanStart();

  portENTER_CRITICAL(&mux);
  if (err.length()) st.fails++;
  else {
    st.reads++;
    st.dataMs = millis();
    st.a = be16(info) * 0.01f;
    st.v = (uint16_t)be16(info + 2) * 0.1f;
    st.remAh = be32(info + 4) * 0.001f;
    st.capAh = be32(info + 8) * 0.001f;
    if (gotCells) {
      st.cells = constrain((uint16_t)be16(cells), 0, 16);
      for (int i = 0; i < st.cells; i++) st.cellV[i] = (uint16_t)be16(cells + 2 + 2 * i) * 0.1f;
    }
    if (gotTemps) {
      st.temps = constrain((uint16_t)be16(temps), 0, 16);
      for (int i = 0; i < st.temps; i++) st.tempC[i] = be16(temps + 2 + 2 * i) * 0.1f;
    }
    if (gotModel) {
      memcpy(st.model, model, 16); st.model[16] = 0;
      for (int i = 15; i >= 0 && (st.model[i] == ' ' || !st.model[i]); i--) st.model[i] = 0;
    }
  }
  portEXIT_CRITICAL(&mux);
  setErr(err.c_str());
}

// ------------------------------------------------------------------ output
String json() {
  State s;
  portENTER_CRITICAL(&mux); s = st; portEXIT_CRITICAL(&mux);
  uint32_t now = millis();
  String j = "{\"name\":\"" + vannet::jsonEsc(cfgName) + "\",\"on\":" + (cfgOn ? "true" : "false")
           + ",\"rssi\":" + (s.found ? String(s.rssi) : String("null"))
           + ",\"seen\":" + (s.found ? String((now - s.seenMs) / 1000) : String("null"))
           + ",\"age\":" + (s.dataMs ? String((now - s.dataMs) / 1000) : String("null"))
           + ",\"err\":\"" + vannet::jsonEsc(s.err) + "\",\"model\":\"" + vannet::jsonEsc(s.model) + "\"";
  if (s.dataMs) {
    j += ",\"v\":" + jnum(s.v, 1) + ",\"a\":" + jnum(s.a, 2) + ",\"rem_ah\":" + jnum(s.remAh, 1)
       + ",\"cap_ah\":" + jnum(s.capAh, 1) + ",\"soc\":" + jnum(s.capAh > 0 ? 100.0f * s.remAh / s.capAh : NAN, 1) + ",\"cells\":[";
    for (int i = 0; i < s.cells; i++) j += (i ? "," : "") + jnum(s.cellV[i], 1);
    j += "],\"temps\":[";
    for (int i = 0; i < s.temps; i++) j += (i ? "," : "") + jnum(s.tempC[i], 1);
    j += "]";
  }
  return j + "}";
}

String settingsHtml() {
  return "<label>Bluetooth name <input class=w name=rname value=\"" + htmlEsc(cfgName) + "\"></label>"
         "<label>Modbus address <input name=rid inputmode=numeric value=" + String(cfgId) + "></label>"
         "<small>255 for the battery's own Bluetooth (try 48 if it never answers).</small>"
         "<label>Read every <input name=rsec inputmode=numeric value=" + String(cfgEveryS) + "> s</label>"
         "<label><input type=checkbox name=ron value=1" + (cfgOn ? " checked" : "") + "> read the battery</label>";
}

String statusHtml() {
  State s;
  portENTER_CRITICAL(&mux); s = st; portEXIT_CRITICAL(&mux);
  uint32_t now = millis();
  String h = s.found ? String(s.rssi) + " dBm, heard " + agoText(now - s.seenMs) + " &middot; " : String();
  if (s.dataMs) h += "<span class=ok>read " + agoText(now - s.dataMs) + (s.model[0] ? String(": ") + htmlEsc(s.model) : String()) + "</span>";
  if (s.err[0]) h += String(s.dataMs ? " &middot; " : "") + "<span class=warn>" + htmlEsc(s.err) + "</span>";
  return h;
}

void saveSettings(WebServer& sv) {
  String n = sv.arg("rname"); n.trim();
  if (n != cfgName) { portENTER_CRITICAL(&mux); st.found = false; st.model[0] = 0; portEXIT_CRITICAL(&mux); }
  cfgName = n;
  cfgId = constrain(sv.arg("rid").toInt(), 1, 255);
  cfgEveryS = constrain(sv.arg("rsec").toInt(), 10, 3600);
  cfgOn = sv.arg("ron") == "1";
  prefs.putString("name", cfgName);
  prefs.putUChar("id", cfgId);
  prefs.putUInt("every", cfgEveryS);
  prefs.putBool("on", cfgOn);
  polledOnce = false;                         // read again now
}

void printStatus() {
  State s;
  portENTER_CRITICAL(&mux); s = st; portEXIT_CRITICAL(&mux);
  Serial.printf("  renogy %s (id %u) %s, %s | reads %lu ok, %lu failed | %s\n", cfgName.c_str(), cfgId,
                cfgOn ? "on" : "off", s.found ? (String(s.rssi) + " dBm").c_str() : "not heard",
                (unsigned long)s.reads, (unsigned long)s.fails, s.err);
  if (s.dataMs) Serial.printf("    %.1f V %.2f A, %.1f of %.1f Ah, %d cells, %d temps, model %s\n", s.v, s.a, s.remAh, s.capAh,
                              s.cells, s.temps, s.model);
}

}  // namespace renogy
