// victron.cpp -- see victron.h.
//
// Advertisement layout (manufacturer data, after the company id E1 02), as decoded by the victron-ble and
// esphome-victron_ble projects:
//   [0] 0x10 = product advertisement   [1] ?   [2..3] model id (LE)   [4] readout type (1 solar charger, 2 battery monitor)
//   [5..6] counter / IV (LE)   [7] first byte of the key (to tell a wrong key)   [8..] AES-128-CTR encrypted readings
// The CTR counter block is the 16-bit IV as a little-endian 128-bit number. The readings are bit fields, LSB first.
#include "victron.h"
#include "util.h"
#include <Preferences.h>
#include <VanNet.h>
#include <mbedtls/aes.h>

namespace victron {

const int MAX_DEV = 8;                          // saved devices + others heard nearby (a campground can be busy)
const uint32_t DROP_MS = 5UL * 60 * 1000;       // an unsaved device not heard for 5 min drops off the list

struct Dev {
  bool used, saved, keyed;
  char mac[18];
  char name[25];                                // set on the setup page ("" = the advertised name)
  char advName[25];
  uint8_t key[16];
  uint16_t model;
  uint8_t type;                                 // readout type, 0 = no Instant Readout heard yet
  int rssi;
  uint32_t seenMs, dataMs;                      // last advertisement / last decoded readings (0 = never)
  bool keyWrong;
  // readings, NAN / -1 = not available
  uint8_t state, error;                         // solar charger
  float v, a, kwh, loadA;
  int pvW;
  float soc, ah, aux;                           // battery monitor (v, a shared)
  int ttg;                                      // minutes, -1 = infinite / not available
  uint16_t alarm;
  uint8_t auxMode;                              // 0 starter V, 1 midpoint V, 2 temperature, 3 none
};
static Dev dev[MAX_DEV];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;   // onAdvert runs in the Bluetooth task
static Preferences prefs;                                  // "victron": m<i> mac, n<i> name, k<i> key (slot i)

static int findMac(const char* mac) {
  for (int i = 0; i < MAX_DEV; i++) if (dev[i].used && !strcasecmp(dev[i].mac, mac)) return i;
  return -1;
}

void begin() {
  prefs.begin("victron", false);
  for (int i = 0; i < MAX_DEV; i++) {
    String k = "m" + String(i);
    if (!prefs.isKey(k.c_str())) continue;
    Dev& d = dev[i];
    d.used = d.saved = true;
    strlcpy(d.mac, prefs.getString(k.c_str()).c_str(), sizeof(d.mac));
    strlcpy(d.name, prefs.getString(("n" + String(i)).c_str(), "").c_str(), sizeof(d.name));
    d.keyed = prefs.getBytes(("k" + String(i)).c_str(), d.key, 16) == 16;
  }
}

// ------------------------------------------------------------------ decoding
struct Bits {
  const uint8_t* d; size_t n; size_t pos = 0;
  uint32_t u(int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; i++, pos++)
      if ((pos >> 3) < n && ((d[pos >> 3] >> (pos & 7)) & 1)) v |= 1UL << i;
    return v;
  }
  int32_t s(int bits) { uint32_t v = u(bits); if (v & (1UL << (bits - 1))) v |= ~0UL << bits; return (int32_t)v; }
};

static void decrypt(const uint8_t key[16], uint16_t iv, const uint8_t* in, size_t n, uint8_t* out) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  mbedtls_aes_setkey_enc(&aes, key, 128);
  uint32_t ctr = iv;
  for (size_t off = 0; off < n; off += 16, ctr++) {
    uint8_t blk[16] = {(uint8_t)ctr, (uint8_t)(ctr >> 8), (uint8_t)(ctr >> 16)}, ks[16];
    mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, blk, ks);
    for (size_t i = 0; i < 16 && off + i < n; i++) out[off + i] = in[off + i] ^ ks[i];
  }
  mbedtls_aes_free(&aes);
}

static void decodeSolar(const uint8_t* p, size_t n, Dev& r) {
  Bits b{p, n};
  r.state = b.u(8);
  r.error = b.u(8);
  int32_t v = b.s(16), a = b.s(16);
  uint32_t y = b.u(16), pw = b.u(16), la = b.u(9);
  r.v = v == 0x7FFF ? NAN : v * 0.01f;
  r.a = a == 0x7FFF ? NAN : a * 0.1f;          // into the battery
  r.kwh = y == 0xFFFF ? NAN : y * 0.01f;       // yield today
  r.pvW = pw == 0xFFFF ? -1 : (int)pw;
  r.loadA = la == 0x1FF ? NAN : la * 0.1f;     // load output (the 100/30 and 75/15 have none)
}

static void decodeMonitor(const uint8_t* p, size_t n, Dev& r) {
  Bits b{p, n};
  uint32_t ttg = b.u(16);
  int32_t v = b.s(16);
  r.alarm = b.u(16);
  uint32_t aux = b.u(16);
  r.auxMode = b.u(2);
  int32_t a = b.s(22);
  uint32_t ah = b.u(20), soc = b.u(10);
  r.ttg = ttg == 0xFFFF ? -1 : (int)ttg;
  r.v = v == 0x7FFF ? NAN : v * 0.01f;
  r.a = a == 0x1FFFFF ? NAN : a * 0.001f;       // + = charging
  r.ah = ah == 0xFFFFF ? NAN : -(float)ah * 0.1f;
  r.soc = soc == 0x3FF ? NAN : soc * 0.1f;
  r.aux = r.auxMode == 0 ? (int16_t)aux * 0.01f : r.auxMode == 1 ? aux * 0.01f : r.auxMode == 2 ? aux * 0.01f - 273.15f : NAN;
}

void onAdvert(const char* mac, const uint8_t* m, size_t len, const char* name, int rssi) {
  if (len < 4 || m[0] != 0xE1 || m[1] != 0x02) return;
  bool rec = len >= 11 && m[2] == 0x10;
  uint32_t now = millis();
  uint8_t key[16];
  bool keyed = false;
  portENTER_CRITICAL(&mux);
  int i = findMac(mac);
  if (i < 0) {                                   // new: a free slot, else the unsaved one heard longest ago
    for (int j = 0; j < MAX_DEV; j++) if (!dev[j].used) { i = j; break; }
    if (i < 0) for (int j = 0; j < MAX_DEV; j++)
      if (!dev[j].saved && (i < 0 || now - dev[j].seenMs > now - dev[i].seenMs)) i = j;
    if (i >= 0) { dev[i] = Dev(); dev[i].used = true; strlcpy(dev[i].mac, mac, sizeof(dev[i].mac)); }
  }
  if (i >= 0) {
    Dev& d = dev[i];
    d.rssi = rssi; d.seenMs = now;
    if (name && *name) strlcpy(d.advName, name, sizeof(d.advName));
    if (rec) { d.model = m[4] | (m[5] << 8); d.type = m[6]; }
    keyed = d.keyed;
    memcpy(key, d.key, 16);
    if (rec && keyed) d.keyWrong = m[9] != key[0];
    keyed = keyed && !d.keyWrong;
  }
  portEXIT_CRITICAL(&mux);
  if (i < 0 || !rec || !keyed) return;

  uint8_t plain[16] = {0};
  size_t n = min(len - 10, sizeof(plain));
  decrypt(key, m[7] | (m[8] << 8), m + 10, n, plain);
  Dev r = Dev();
  uint8_t type = m[6];
  if (type == 1) decodeSolar(plain, n, r);
  else if (type == 2) decodeMonitor(plain, n, r);
  else return;                                   // other Victron products: listed, not decoded

  portENTER_CRITICAL(&mux);
  i = findMac(mac);                              // the slot may have changed while decoding
  if (i >= 0) {
    Dev& d = dev[i];
    d.dataMs = now ? now : 1;
    d.state = r.state; d.error = r.error; d.v = r.v; d.a = r.a; d.kwh = r.kwh; d.loadA = r.loadA; d.pvW = r.pvW;
    d.soc = r.soc; d.ah = r.ah; d.aux = r.aux; d.ttg = r.ttg; d.alarm = r.alarm; d.auxMode = r.auxMode;
  }
  portEXIT_CRITICAL(&mux);
}

// ------------------------------------------------------------------ names
static const char* product(const Dev& d) {
  switch (d.model) {                             // the ones in this van (VictronConnect export, 2026-10-11)
    case 0xA053: return "SmartSolar MPPT 75/15";
    case 0xA056: return "SmartSolar MPPT 100/30";
    case 0xA389: return "SmartShunt 500A/50mV";
  }
  switch (d.type) {
    case 0: return "Victron device (no Instant Readout heard)";
    case 1: return "solar charger";
    case 2: return "battery monitor";
    case 3: return "inverter";
    case 4: return "DC-DC charger";
    case 5: return "Smart Lithium battery";
    case 8: return "AC charger";
    case 9: return "Smart BatteryProtect";
  }
  return "Victron device";
}

static String showName(const Dev& d) { return d.name[0] ? d.name : d.advName[0] ? d.advName : product(d); }

static const char* stateText(uint8_t s) {
  switch (s) {
    case 0: return "off";          case 1: return "low power";    case 2: return "fault";
    case 3: return "bulk";         case 4: return "absorption";   case 5: return "float";
    case 6: return "storage";      case 7: return "equalize";     case 9: return "inverting";
    case 11: return "power supply"; case 245: return "starting up"; case 246: return "repeated absorption";
    case 247: return "recondition"; case 248: return "battery safe"; case 252: return "external control";
  }
  return "state ?";
}

static String errorText(uint8_t e) {
  switch (e) {
    case 0: return "";
    case 2: return "battery voltage too high";      case 17: return "charger too hot";
    case 18: return "charger over current";         case 19: return "charger current reversed";
    case 20: return "bulk time limit";              case 21: return "current sensor issue";
    case 26: return "terminals overheated";         case 28: return "power stage issue";
    case 33: return "PV voltage too high";          case 34: return "PV current too high";
    case 38: case 39: return "input shutdown";      case 65: case 66: return "network / sync issue";
    case 67: return "BMS connection lost";          case 68: return "network misconfigured";
    case 116: return "calibration data lost";       case 117: return "firmware incompatible";
    case 119: return "settings invalid";
  }
  return "error " + String(e);
}

static String alarmText(uint16_t a) {
  static const char* bits[] = {"low voltage", "high voltage", "low state of charge", "low starter voltage",
                               "high starter voltage", "low temperature", "high temperature", "midpoint voltage"};
  String s;
  for (int i = 0; i < 8; i++) if (a & (1 << i)) { if (s.length()) s += ", "; s += bits[i]; }
  if (a & 0xFF00) { if (s.length()) s += ", "; s += "alarm 0x" + String(a, HEX); }
  return s;
}

// ------------------------------------------------------------------ output
static void snapshot(Dev* out) {
  uint32_t now = millis();
  portENTER_CRITICAL(&mux);
  for (int i = 0; i < MAX_DEV; i++)
    if (dev[i].used && !dev[i].saved && now - dev[i].seenMs > DROP_MS) dev[i].used = false;
  memcpy(out, dev, sizeof(dev));
  portEXIT_CRITICAL(&mux);
}

String json() {
  Dev d[MAX_DEV];
  snapshot(d);
  uint32_t now = millis();
  String s = "[";
  bool first = true;
  for (int i = 0; i < MAX_DEV; i++) {
    const Dev& x = d[i];
    if (!x.used) continue;
    if (!first) s += ',';
    first = false;
    s += "{\"mac\":\"" + String(x.mac) + "\",\"name\":\"" + vannet::jsonEsc(showName(x)) + "\",\"product\":\"" + product(x)
       + "\",\"type\":" + x.type + ",\"saved\":" + (x.saved ? "true" : "false") + ",\"key\":" + (x.keyed ? "true" : "false")
       + ",\"key_wrong\":" + (x.keyWrong ? "true" : "false") + ",\"rssi\":" + x.rssi
       + ",\"seen\":" + (now - x.seenMs) / 1000 + ",\"age\":" + (x.dataMs ? String((now - x.dataMs) / 1000) : String("null"));
    if (x.dataMs && x.type == 1)
      s += ",\"state\":\"" + String(stateText(x.state)) + "\",\"err\":\"" + errorText(x.error) + "\",\"bat_v\":" + jnum(x.v, 2)
         + ",\"bat_a\":" + jnum(x.a, 1) + ",\"pv_w\":" + (x.pvW < 0 ? String("null") : String(x.pvW))
         + ",\"yield_kwh\":" + jnum(x.kwh, 2) + ",\"load_a\":" + jnum(x.loadA, 1);
    if (x.dataMs && x.type == 2)
      s += ",\"soc\":" + jnum(x.soc, 1) + ",\"bat_v\":" + jnum(x.v, 2) + ",\"bat_a\":" + jnum(x.a, 2)
         + ",\"ah\":" + jnum(x.ah, 1) + ",\"ttg\":" + (x.ttg < 0 ? String("null") : String(x.ttg))
         + ",\"alarm\":\"" + alarmText(x.alarm) + "\",\"aux_mode\":" + x.auxMode + ",\"aux\":" + jnum(x.aux, 2);
    s += "}";
  }
  return s + "]";
}

String setupHtml() {
  Dev d[MAX_DEV];
  snapshot(d);
  uint32_t now = millis();
  String s;
  for (int pass = 0; pass < 2; pass++)            // saved devices first, then the others heard nearby
    for (int i = 0; i < MAX_DEV; i++) {
      const Dev& x = d[i];
      if (!x.used || x.saved != (pass == 0)) continue;
      String st;
      if (!x.seenMs) st = "<span class=bad>not heard since the hub started</span>";
      else {
        st = String(x.rssi) + " dBm, heard " + agoText(now - x.seenMs) + " &middot; ";
        if (!x.type) st += "<span class=warn>Instant Readout is off</span>";
        else if (!x.keyed) st += "<span class=warn>no key yet</span>";
        else if (x.keyWrong) st += "<span class=bad>wrong key: it is another device's, or mistyped</span>";
        else if (x.dataMs) st += "<span class=ok>key OK, readings " + agoText(now - x.dataMs) + "</span>";
        else if (x.type > 2) st += "key saved; this product's readings aren't decoded yet";
        else st += "key saved, waiting for readings";
      }
      s += "<form method=POST action=/api/victron class=dev><b>" + htmlEsc(showName(x)) + "</b> &middot; " + product(x)
         + "<br><small>" + x.mac + " &middot; " + st + "</small>"
         + "<input type=hidden name=mac value=\"" + x.mac + "\">"
         + "<input class=w name=name maxlength=24 placeholder=\"name, e.g. 200W Aft\" value=\"" + htmlEsc(x.name) + "\">"
         + "<input class=w name=key autocomplete=off autocapitalize=off autocorrect=off spellcheck=false placeholder=\""
         + (x.keyed ? "key saved (empty = keep it)" : "encryption key, 32 characters") + "\">"
         + "<div class=two><button>Save</button>" + (x.saved ? "<button name=forget value=1 class=sec>Forget</button>" : "")
         + "</div></form>";
    }
  if (!s.length()) s = "<p class=warn>No Victron devices heard yet. Bluetooth reaches about 10 m.</p>";
  return s;
}

String save(const String& mac, const String& name, const String& keyHex, bool forget) {
  String k;
  for (char c : keyHex) if (isxdigit((unsigned char)c)) k += c; else if (c != ' ' && c != ':' && c != '-') return "the key has a character that isn't 0-9 / a-f";
  if (k.length() && k.length() != 32) return "the key is 32 characters (0-9, a-f); this one has " + String(k.length());
  uint8_t key[16];
  for (int i = 0; i < 16 && k.length(); i++) key[i] = strtoul(k.substring(2 * i, 2 * i + 2).c_str(), nullptr, 16);

  portENTER_CRITICAL(&mux);
  int i = findMac(mac.c_str());
  portEXIT_CRITICAL(&mux);
  if (i < 0) return "that device is no longer in the list";
  String si(i);
  if (forget) {
    prefs.remove(("m" + si).c_str()); prefs.remove(("n" + si).c_str()); prefs.remove(("k" + si).c_str());
  } else {
    prefs.putString(("m" + si).c_str(), mac);
    prefs.putString(("n" + si).c_str(), name);
    if (k.length()) prefs.putBytes(("k" + si).c_str(), key, 16);
  }
  portENTER_CRITICAL(&mux);
  Dev& d = dev[i];
  if (forget) { d.saved = d.keyed = false; d.name[0] = 0; }
  else {
    d.saved = true;
    strlcpy(d.name, name.c_str(), sizeof(d.name));
    if (k.length()) { memcpy(d.key, key, 16); d.keyed = true; }
  }
  d.keyWrong = false; d.dataMs = 0;
  portEXIT_CRITICAL(&mux);
  Serial.printf("victron %s: %s\n", mac.c_str(), forget ? "forgotten" : k.length() ? "name + key saved" : "name saved");
  return "";
}

void printStatus() {
  Dev d[MAX_DEV];
  snapshot(d);
  uint32_t now = millis();
  for (int i = 0; i < MAX_DEV; i++) {
    const Dev& x = d[i];
    if (!x.used) continue;
    Serial.printf("  victron %s %-24s %-26s %4d dBm key:%s", x.mac, showName(x).c_str(), product(x), x.rssi,
                  !x.keyed ? "none" : x.keyWrong ? "WRONG" : "ok");
    if (x.dataMs && x.type == 1) Serial.printf(" | %s, PV %d W, %.2f V %.1f A, today %.2f kWh %s", stateText(x.state), x.pvW,
                                               x.v, x.a, x.kwh, errorText(x.error).c_str());
    if (x.dataMs && x.type == 2) Serial.printf(" | %.1f %%, %.2f V %.2f A, %.1f Ah", x.soc, x.v, x.a, x.ah);
    if (x.dataMs) Serial.printf(" (%lu s old)", (unsigned long)((now - x.dataMs) / 1000));
    Serial.println();
  }
}

}  // namespace victron
