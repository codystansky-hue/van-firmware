// maxxfan.ino -- MaxxFan BLDC retrofit controller, ESP32-C3 SuperMini (HW-466AB)
// v1 (2026-10-03): lid control through the DRV8871 (+ phone web page, BOOT button, serial).
// v2 (2026-10-09, never flashed): joins the van router (Starlink) saved on the page, MaxxFan hotspot only as a fallback;
//   firmware updates over Wi-Fi with automatic rollback.
// v3 (2026-10-09): fan wiring picked on the setup page (one firmware for every build stage); knob + direction switch read
//   by the ESP so the knob and the phone both work, whichever was touched last wins; optional panel lid button; passwords
//   kept on the ESP, not in git; a fan reversal no longer blocks the loop. M (speed pulses) moved G0 -> G10.
//
// Arduino core "esp32" by Espressif 3.x (3.3.12). Board settings are in sketch.yaml: ESP32C3 Dev Module, USB CDC On Boot
// Enabled, Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA)". Wiring, flashing and use: README.md next to this file.

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <esp_ota_ops.h>

// ------------------------------------------------------------------ configuration
#define FW_VERSION "v3"
const char* FW_BUILD = FW_VERSION " " __DATE__ " " __TIME__;

const char* AP_SSID  = "MaxxFan";     // the ESP's own hotspot (fallback when the van router is unreachable)
const char* HOSTNAME = "maxxfan";     // http://maxxfan.local on the van router (mDNS)
const char* OTA_USER = "maxxfan";     // setup page login; its password is also the PC-upload password
// first-boot passwords only: change them on the setup page, which keeps them on the ESP (not in git)
const char* DEFAULT_AP_PASS  = "maxxfan123";
const char* DEFAULT_OTA_PASS = "maxxfan-ota";

const uint32_t AP_FALLBACK_MS = 20000;            // router unreachable this long -> hotspot on
const uint32_t AP_IDLE_OFF_MS = 60000;            // router up + nobody on the hotspot this long -> hotspot off
const uint32_t TRIAL_MS = 10UL * 60 * 1000;       // new firmware must answer a web request within this or roll back

// fan wiring, picked on the setup page (applied at restart)
enum FanMode : uint8_t {
  FAN_KNOB_DIRECT = 0,                // knob harness (H11) in the aqx plug; the ESP leaves the fan pins alone
  FAN_PHONE       = 1,                // 74AHCT125 buffer fitted, ESP ribbon in the aqx plug: the phone runs the fan
  FAN_PHONE_KNOB  = 2                 // + knob harness in the panel port: knob and phone, whichever was touched last
};

// pins (ESP32-C3 GPIO numbers = the "G" labels on the SuperMini)
const int PIN_LID_IN1 = 6;            // G6 -> DRV8871 IN1  (harness H5, white)
const int PIN_LID_IN2 = 5;            // G5 -> DRV8871 IN2  (harness H5, grey)
const int PIN_LED     = 8;            // on-board blue LED, LOW = on
const int PIN_BOOT    = 9;            // on-board BOOT button, LOW = pressed (read only after boot)
const int PIN_LID_BTN = 20;           // G20 <- optional panel lid button to GND, works like BOOT
const int PIN_FAN_VR  = 4;            // G4 -> buffer 1A -> aqx VR  (PWM speed)                  [fan modes 1, 2]
const int PIN_FAN_EL  = 1;            // G1 -> buffer 2A -> aqx EL  (HIGH = run)                 [1, 2]
const int PIN_FAN_ZF  = 3;            // G3 -> buffer 3A -> aqx ZF  (HIGH = forward = exhaust)   [1, 2]
const int PIN_FAN_M   = 10;           // G10 <- 10k/20k divider <- aqx M (speed pulses)          [1, 2]  (G0 up to v2)
const int PIN_KNOB    = 0;            // G0 <- 12k/10k divider <- knob wiper, 0-5 V -> 0-2.27 V   [2]
const int PIN_DIR     = 7;            // G7 <- 10k/20k divider <- direction switch, 5 V = forward [2]

const uint32_t KNOB_OFF_MV  = 120;    // below this the knob is at its click = off
const uint32_t KNOB_FULL_MV = 2150;   // knob fully up (the divider gives ~2270 mV; the setup page shows the reading)
const uint32_t LID_PWM_HZ   = 20000;  // DRV8871 input PWM for the soft start (inaudible)
const uint32_t FAN_PWM_HZ   = 20000;  // aqx VR accepts 1-20 kHz PWM
const uint32_t FLIP_WAIT_MS = 3000;   // fan reversal: drive off this long before ZF flips

// ------------------------------------------------------------------ settings (kept in flash, survive updates)
Preferences prefs;
struct Settings {
  uint32_t travelMs;     // full close -> open time (calibrate!)
  uint32_t overMs;       // extra run at each end so the lid seats fully
  uint32_t rampMs;       // soft start
  bool     invert;       // swap if OPEN closes the lid
  float    lastPos;      // 0 = closed .. 100 = open, -1 = unknown
  uint8_t  fanMode;      // FanMode
} cfg;
String staSsid, staPass;              // van router, set on the fan page ("" = hotspot only)
String apPass, otaPass;               // set on the setup page

void loadSettings() {
  prefs.begin("maxxfan", false);
  cfg.travelMs = prefs.getUInt("travel", 12000);
  cfg.overMs   = prefs.getUInt("over", 600);
  cfg.rampMs   = prefs.getUInt("ramp", 300);
  cfg.invert   = prefs.getBool("invert", false);
  cfg.lastPos  = prefs.getFloat("pos", -1.0f);
  cfg.fanMode  = prefs.getUChar("fanmode", FAN_KNOB_DIRECT);
  if (cfg.fanMode > FAN_PHONE_KNOB) cfg.fanMode = FAN_KNOB_DIRECT;
  staSsid      = prefs.getString("ssid", "");
  staPass      = prefs.getString("wpass", "");
  apPass       = prefs.getString("appass", DEFAULT_AP_PASS);
  otaPass      = prefs.getString("otapass", DEFAULT_OTA_PASS);
}
void saveSettings() {
  prefs.putUInt("travel", cfg.travelMs);
  prefs.putUInt("over", cfg.overMs);
  prefs.putUInt("ramp", cfg.rampMs);
  prefs.putBool("invert", cfg.invert);
}
void savePos(float p) { prefs.putFloat("pos", p); }

// ------------------------------------------------------------------ fan request (what was asked, by whom)
int  fanSpeed = 0;                    // requested 0..100 %
bool fanForward = true;               // requested direction (forward = exhaust)
bool fanFromKnob = false;             // the request came from the knob / switch, else from the phone

// ------------------------------------------------------------------ lid motor (DRV8871)
// IN1 IN2: 0 0 coast, PWM 0 drive A, 0 PWM drive B, 1 1 brake
enum LidMove { LID_IDLE, LID_OPENING, LID_CLOSING };
LidMove lidMove = LID_IDLE;
float    lidPos = -1.0f;             // estimated position, -1 unknown
float    moveStartPos = 0;
uint32_t moveStartMs = 0, moveDurMs = 0;
bool     moveFull = false;           // run to the end stop (adds overMs)
uint32_t brakeUntil = 0;

void lidOutputs(int dir, uint8_t duty) {      // dir +1 open, -1 close, 0 coast, 2 brake
  int a = PIN_LID_IN1, b = PIN_LID_IN2;
  if (cfg.invert) { a = PIN_LID_IN2; b = PIN_LID_IN1; }
  const uint32_t FULL = 256;            // 8-bit LEDC: 256 = 100 % (steady high)
  uint32_t d = (duty >= 255) ? FULL : duty;
  if (dir == 2) { ledcWrite(a, FULL); ledcWrite(b, FULL); return; }
  if (dir == 0) { ledcWrite(a, 0);    ledcWrite(b, 0);    return; }
  if (dir > 0)  { ledcWrite(a, d); ledcWrite(b, 0); }
  else          { ledcWrite(a, 0); ledcWrite(b, d); }
}

float currentPos() {
  if (lidMove == LID_IDLE || lidPos < 0) return lidPos;
  if ((int32_t)(millis() - moveStartMs) < 0) return moveStartPos;     // still in the reversal pause
  float d = 100.0f * (millis() - moveStartMs) / (float)cfg.travelMs;
  float p = moveStartPos + (lidMove == LID_OPENING ? d : -d);
  return constrain(p, 0.0f, 100.0f);
}

void lidStop(const char* why) {
  if (lidMove != LID_IDLE) {
    lidPos = currentPos();
    lidOutputs(2, 255);                 // brake briefly, then coast (handled in loop)
    brakeUntil = millis() + 150;
    lidMove = LID_IDLE;
    savePos(lidPos);
    Serial.printf("lid stop (%s) at ~%.0f%%\n", why, lidPos);
  }
}

void lidStart(int dir, uint32_t durMs, bool full) {
  bool wasMoving = (lidMove != LID_IDLE);
  if (wasMoving) lidStop("reverse");
  moveStartPos = (lidPos < 0) ? (dir > 0 ? 0.0f : 100.0f) : lidPos;
  if (lidPos < 0) lidPos = moveStartPos;  // unknown: assume the far end so a full move covers everything
  moveDurMs = durMs + (full ? cfg.overMs : 0);
  moveFull = full;
  moveStartMs = millis() + (wasMoving ? 300 : 0);   // brake 300 ms before driving the other way
  lidMove = dir > 0 ? LID_OPENING : LID_CLOSING;
  Serial.printf("lid %s for %lu ms\n", dir > 0 ? "OPEN" : "CLOSE", (unsigned long)moveDurMs);
}

void lidOpen()  { float p = (lidPos < 0) ? 0 : lidPos; lidStart(+1, (uint32_t)((100.0f - p) / 100.0f * cfg.travelMs), true); }
void lidClose() {
  if (!fanFromKnob) fanSpeed = 0;       // closing cancels a phone fan request (a knob that is up stays up)
  float p = (lidPos < 0) ? 100 : lidPos;
  lidStart(-1, (uint32_t)(p / 100.0f * cfg.travelMs), true);
}
void lidJog(int dir, uint32_t ms) { lidStart(dir, ms, false); }

void lidLoop() {
  uint32_t now = millis();
  if (lidMove == LID_IDLE) {
    if (brakeUntil && (int32_t)(now - brakeUntil) >= 0) { lidOutputs(0, 0); brakeUntil = 0; }   // brake -> coast
    return;
  }
  brakeUntil = 0;
  if ((int32_t)(now - moveStartMs) < 0) { lidOutputs(2, 255); return; }   // reversal pause: hold the brake
  uint32_t el = now - moveStartMs;
  uint8_t duty = (cfg.rampMs && el < cfg.rampMs) ? (uint8_t)(80 + 175UL * el / cfg.rampMs) : 255;
  lidOutputs(lidMove == LID_OPENING ? +1 : -1, duty);
  if (el >= moveDurMs) {
    bool opening = (lidMove == LID_OPENING);
    lidStop("done");
    if (moveFull) { lidPos = opening ? 100.0f : 0.0f; savePos(lidPos); }
  }
}

// ------------------------------------------------------------------ fan (aqx through the 74AHCT125), fan modes 1 and 2
// The fan only runs with the lid open and still; a new request opens the lid first (not the knob position found at boot).
// A reversal cuts the drive and waits FLIP_WAIT_MS before ZF flips. Mode 2: turning the knob (5 % or more) or flipping
// the switch hands the fan to the panel; a phone command hands it back to the phone.
bool fanEsp() { return cfg.fanMode != FAN_KNOB_DIRECT; }
bool zfForward = true;                // direction on ZF now
int  fanOut = 0;                      // speed on VR now
uint32_t flipAt = 0;                  // reversal in progress: ZF flips at this time
volatile uint32_t mPulses = 0;
float mHz = 0;
float knobMv = 0;
int  knobPct = 0, knobTaken = 0;      // knob now / knob when the fan was last set
bool dirFwd = true, dirRaw = true;    // direction switch: debounced / raw
uint32_t knobT = 0, dirChangeMs = 0;

void IRAM_ATTR onM() { mPulses = mPulses + 1; }   // single writer (this ISR); C++20 deprecates ++ on volatile

void setFan(int speed, bool forward, bool fromKnob, bool mayOpenLid = true) {
  if (!fanEsp()) return;
  fanSpeed = constrain(speed, 0, 100); fanForward = forward; fanFromKnob = fromKnob;
  if (!fromKnob) knobTaken = knobPct;           // the knob has to move again to take over
  if (mayOpenLid && fanSpeed > 0 && !(lidPos >= 95.0f) && lidMove != LID_OPENING) lidOpen();
}

void fanApply() {
  if (!fanEsp()) return;
  uint32_t now = millis();
  int s = (lidMove == LID_IDLE && lidPos >= 95.0f) ? fanSpeed : 0;     // interlock
  if (fanForward != zfForward) {                                       // never reverse at speed
    if (!flipAt) flipAt = now + ((fanOut > 0 || mHz > 1) ? FLIP_WAIT_MS : 1);
    if ((int32_t)(now - flipAt) < 0) s = 0;
    else { zfForward = fanForward; flipAt = 0; }
  } else flipAt = 0;
  fanOut = s;
  digitalWrite(PIN_FAN_ZF, zfForward ? HIGH : LOW);
  digitalWrite(PIN_FAN_EL, s > 0 ? HIGH : LOW);
  ledcWrite(PIN_FAN_VR, (uint32_t)(1023UL * s / 100));
}

int knobToPct(float mv) {
  if (mv < KNOB_OFF_MV) return 0;
  return constrain((int)lroundf(100.0f * (mv - KNOB_OFF_MV) / (KNOB_FULL_MV - KNOB_OFF_MV)), 1, 100);
}
void knobRead() {
  knobMv += 0.25f * ((float)analogReadMilliVolts(PIN_KNOB) - knobMv);
  knobPct = knobToPct(knobMv);
}

void knobLoop() {
  if (cfg.fanMode != FAN_PHONE_KNOB) return;
  uint32_t now = millis();
  if (now - knobT < 20) return;
  knobT = now;
  knobRead();
  bool d = digitalRead(PIN_DIR) == HIGH;
  if (d != dirRaw) { dirRaw = d; dirChangeMs = now; }
  bool flipped = (dirRaw != dirFwd && now - dirChangeMs > 50);
  if (flipped) dirFwd = dirRaw;
  int step = abs(knobPct - knobTaken);
  bool deliberate = step >= 5 || flipped || (knobTaken == 0 && knobPct > 0);   // a real turn: may open the lid
  bool follow = fanFromKnob && (step >= 2 || (knobPct == 0) != (knobTaken == 0));
  if (deliberate || follow) {
    if (!fanFromKnob) Serial.println("fan: the knob took over");
    knobTaken = knobPct;
    setFan(knobPct, dirFwd, true, deliberate);
  }
}

// ------------------------------------------------------------------ buttons: BOOT or panel lid button, tap = open / close / stop
uint32_t btnDownMs = 0; bool btnWas = false;
void buttonLoop() {
  bool down = digitalRead(PIN_BOOT) == LOW || digitalRead(PIN_LID_BTN) == LOW;
  if (down && !btnWas) btnDownMs = millis();
  if (!down && btnWas && millis() - btnDownMs > 40) {
    if (lidMove != LID_IDLE) lidStop("button");
    else if (lidPos >= 50.0f) lidClose();
    else lidOpen();
  }
  btnWas = down;
}

// ------------------------------------------------------------------ Wi-Fi: van router first, own hotspot as fallback
// With a router saved on the page the ESP joins it and the phone stays on the router: http://maxxfan.local.
// Router unreachable for 20 s -> the MaxxFan hotspot comes on (http://192.168.4.1); it goes off again once the router is
// back and nobody has been on the hotspot for a minute. Retries are done here, not by the Wi-Fi driver, and wait while a
// phone is on the hotspot: a join attempt scans every channel and would make the hotspot drop out under it.
bool apOn = false, staUp = false;
uint32_t staDownMs = 0, staTryMs = 0, apBusyMs = 0;

void apStart() {
  if (apOn) return;
  WiFi.softAP(AP_SSID, apPass.c_str());
  WiFi.setTxPower(WIFI_POWER_8_5dBm);   // SuperMini boards often fail to connect at full power
  apOn = true; apBusyMs = millis();
  Serial.printf("hotspot \"%s\" on: http://%s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}
void apStop() {
  if (!apOn) return;
  WiFi.softAPdisconnect(true);
  apOn = false;
  Serial.println("hotspot off (router is up)");
}
void staJoin() {
  staTryMs = millis();
  WiFi.begin(staSsid.c_str(), staPass.c_str());
}

void wifiLoop() {
  uint32_t now = millis();
  if (!staSsid.length()) { apStart(); return; }          // no router saved: hotspot only
  bool up = WiFi.status() == WL_CONNECTED;
  if (up != staUp) {
    staUp = up;
    if (up) Serial.printf("joined \"%s\": http://%s.local = http://%s\n", staSsid.c_str(), HOSTNAME,
                          WiFi.localIP().toString().c_str());
    else { staDownMs = now; Serial.println("router link down"); }
  }
  bool phoneOnAp = apOn && WiFi.softAPgetStationNum() > 0;
  if (phoneOnAp) apBusyMs = now;
  if (up) { if (apOn && now - apBusyMs > AP_IDLE_OFF_MS) apStop(); return; }
  if (!apOn && now - staDownMs > AP_FALLBACK_MS) apStart();
  if (!phoneOnAp && now - staTryMs > (apOn ? 60000UL : 10000UL)) staJoin();
}

void wifiChanged() {                  // router saved or forgotten on the page
  staUp = false; staDownMs = millis();
  if (staSsid.length()) staJoin();    // a hotspot that is on stays on, so the page can show the result
  else { WiFi.disconnect(); apStart(); }
}

// ------------------------------------------------------------------ firmware updates over Wi-Fi
// Two ways in, both with the setup password: a PC on the same network (arduino-cli / espota, port 3232), or a .bin
// uploaded on http://maxxfan.local/setup. Either one stops the lid and fan first: loop() is blocked while it runs.
// New firmware runs on trial: it must answer a web request (the page, or GET /api/status) within 10 min. If it doesn't,
// or it crashes / loses power before then, the ESP goes back to the previous firmware.
extern "C" bool verifyRollbackLater() { return true; }   // the core must not confirm it at boot; fwTrialLoop() does
bool fwTrial = false, webSeen = false;

void safeStop(const char* why) {      // before an update or a restart
  if (lidMove != LID_IDLE) { lidStop(why); delay(150); }
  lidOutputs(0, 0); brakeUntil = 0;   // coast
  if (fanEsp()) { fanSpeed = 0; fanApply(); }
  Serial.printf("%s: lid and fan stopped\n", why);
}

void fwTrialLoop() {
  if (!fwTrial) return;
  if (webSeen) { esp_ota_mark_app_valid_cancel_rollback(); fwTrial = false; Serial.println("new firmware confirmed"); }
  else if (millis() > TRIAL_MS) {
    Serial.println("new firmware never answered: going back to the previous one");
    delay(100);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

// ------------------------------------------------------------------ web pages
WebServer server(80);

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>MaxxFan</title>
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-title" content="MaxxFan">
<style>body{font-family:system-ui,sans-serif;max-width:460px;margin:16px auto;padding:0 12px;color:#111}
h1{font-size:22px}button{font-size:20px;padding:16px;margin:6px 0;width:100%;border-radius:10px;border:1px solid #999;background:#f2f2f2}
.row{display:flex;gap:8px}.row button{flex:1}.go{background:#dff3e3}.stop{background:#f8d7d7}
#st{font-size:18px;padding:10px;background:#eef3fb;border-radius:8px}input,select{font-size:16px}input{width:90px}
.w{width:100%;box-sizing:border-box;margin:4px 0;padding:6px}#net{margin:2px 0 6px}
fieldset{margin-top:14px;border-radius:8px}small{color:#555}</style></head><body>
<h1>MaxxFan</h1><div id="st">...</div>
<div class="row"><button class="go" onclick="c('open')">OPEN</button><button class="go" onclick="c('close')">CLOSE</button></div>
<button class="stop" onclick="c('stop')">STOP</button>
<div class="row"><button onclick="c('jog_open')">nudge open</button><button onclick="c('jog_close')">nudge close</button></div>
<div id="fan"></div>
<fieldset><legend>Calibrate the lid</legend>
<small>Close the lid fully (nudge if needed), press <b>Set closed</b>, then time a full OPEN with a stopwatch and enter it.</small><br>
full travel ms <input id="tr"> extra ms <input id="ov"><br>
<label><input type="checkbox" id="inv"> swap direction (if OPEN closes)</label><br>
<button onclick="save()">Save settings</button><button onclick="c('set_closed')">Set closed (position = 0 %)</button></fieldset>
<fieldset><legend>Wi-Fi</legend><div id="net">...</div>
<small>Save the van router (Starlink) here and your phone can stay on it: bookmark <b>http://maxxfan.local</b>.
The MaxxFan hotspot then turns off, and comes back by itself when the router is off for 20 s.</small>
<select id="nets" class="w" onchange="ss.value=this.value" hidden></select>
<input id="ss" class="w" placeholder="network name" autocapitalize="off" autocorrect="off" spellcheck="false">
<input id="pw" class="w" type="password" placeholder="password" autocapitalize="off" autocorrect="off">
<div class="row"><button onclick="scan()">scan</button><button onclick="join()">save + join</button></div>
<button onclick="forget()">forget router (hotspot only)</button><small id="wm"></small></fieldset>
<p><small id="fw"></small> &middot; <small><a href="/setup">setup: fan wiring, passwords, firmware update</a></small></p>
<script>
async function c(x){await fetch('/api/lid?cmd='+x);u()}
async function save(){await fetch('/api/settings?travel='+tr.value+'&over='+ov.value+'&invert='+(inv.checked?1:0));u()}
async function f(s,d){await fetch('/api/fan?speed='+s+'&dir='+d);u()}
async function wifi(p){let r=await fetch('/api/wifi',{method:'POST',body:new URLSearchParams(p)});wm.textContent=r.ok?'':await r.text();u()}
function join(){if(ss.value)wifi({ssid:ss.value,pass:pw.value})}
function forget(){if(confirm('Forget the router? The fan will only be on its own MaxxFan hotspot.'))wifi({forget:1})}
async function scan(){wm.textContent='scanning ...';for(let i=0;i<15;i++){let r=await (await fetch('/api/scan')).json();
 if(r.nets){nets.innerHTML='<option value="">pick a network ('+r.nets.length+' found)</option>';
  r.nets.forEach(n=>{let o=document.createElement('option');o.value=o.textContent=n;nets.appendChild(o)});
  nets.hidden=false;wm.textContent='';return}
 await new Promise(z=>setTimeout(z,1000))}wm.textContent='scan failed, type the name'}
let first=true;
async function u(){let r=await (await fetch('/api/status')).json();
let sp=r.fan_out||r.fan,fs=r.fan_mode==0?'fan: knob':'fan '+(sp?sp+'% '+(r.dir=='fwd'?'exhaust':'intake'):'off')
 +(r.fan_wait?' ('+(r.fan_wait=='lid'?'waiting for the lid':'reversing')+')':'')
 +(r.fan_mode==2?' | set by '+r.fan_src+(r.fan_src=='phone'?', knob at '+r.knob+'%':''):'');
st.textContent='lid: '+r.state+(r.pos<0?' (position unknown)':' ~'+Math.round(r.pos)+'% open')+' | '+fs;
let n=!r.wifi_ssid?'no router saved':r.wifi_up?'on '+r.wifi_ssid+' ('+r.wifi_ip+', '+r.wifi_rssi+' dBm)':'trying '+r.wifi_ssid+' ...';
n+=r.ap?' | hotspot on':' | hotspot off';
if(r.wifi_up&&r.ap&&r.ap_clients)n+=' -- switch your phone to '+r.wifi_ssid+' and open http://maxxfan.local';
net.textContent=n;fw.textContent='firmware '+r.fw;
if(first){tr.value=r.travel;ov.value=r.over;inv.checked=r.invert;ss.value=r.wifi_ssid;first=false;
 if(r.fan_mode>0)fan.innerHTML='<fieldset><legend>Fan</legend><div class="row"><button onclick="f(0,\'fwd\')">off</button><button onclick="f(30,\'fwd\')">30%</button><button onclick="f(60,\'fwd\')">60%</button><button onclick="f(100,\'fwd\')">100%</button></div><div class="row"><button onclick="f(50,\'fwd\')">exhaust 50%</button><button onclick="f(50,\'rev\')">intake 50%</button></div>'+(r.fan_mode==2?'<small>The knob and the phone both work: whichever you touched last runs the fan.</small>':'')+'</fieldset>';}}
u();setInterval(u,1000);
</script></body></html>)HTML";

const char SETUP_PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>MaxxFan setup</title>
<style>body{font-family:system-ui,sans-serif;max-width:460px;margin:16px auto;padding:0 12px;color:#111}
h1{font-size:22px}h2{font-size:18px;margin-top:24px;border-bottom:1px solid #999}
button{font-size:20px;padding:14px;margin:10px 0;width:100%;border-radius:10px;border:1px solid #999;background:#dff3e3}
input{font-size:16px}.w{width:100%;box-sizing:border-box;padding:6px;margin:4px 0}label{display:block;margin:10px 0}
small{color:#555}</style></head><body>
<h1>MaxxFan setup</h1><p>Running <b>%FW%</b> &middot; <a href="/">back to the fan</a></p>
<h2>Firmware update</h2>
<form method="POST" action="/update" enctype="multipart/form-data"><input type="file" name="fw" required>
<button>Upload and restart</button></form>
<small>Pick the maxxfan <b>.bin</b> file (maxxfan.ino.bin or maxxfan-v3.bin, not a .merged.bin). The lid and fan stop
first; the upload takes about 20 s. The new firmware is kept
once the fan page loads; if it never does, the ESP goes back to the old firmware by itself after 10 min.</small>
<h2>Fan wiring</h2>
<form method="POST" action="/api/setup">
<label><input type="radio" name="fanmode" value="0" %M0%> <b>Knob only</b>: the knob harness is plugged straight into the
fan board; the ESP only runs the lid.</label>
<label><input type="radio" name="fanmode" value="1" %M1%> <b>Phone</b>: buffer fitted and the ESP ribbon in the fan
board's plug; the phone runs the fan.</label>
<label><input type="radio" name="fanmode" value="2" %M2%> <b>Phone + knob</b>: as Phone, plus the knob harness in the
panel port. Whichever you touched last runs the fan.</label>
<small>Knob now: %KNOB%</small>
<button>Save and restart</button></form>
<h2>Passwords</h2>
<form method="POST" action="/api/setup">
MaxxFan hotspot <input class="w" type="password" name="ap_pass" placeholder="leave empty to keep" autocomplete="new-password">
setup + updates (user maxxfan) <input class="w" type="password" name="ota_pass" placeholder="leave empty to keep" autocomplete="new-password">
<small>8 to 63 characters. Kept on the ESP, through firmware updates.</small>
<button>Save and restart</button></form></body></html>)HTML";

String jsonEsc(const String& s) {
  String o;
  for (char ch : s) { if (ch == '"' || ch == '\\') o += '\\'; if ((uint8_t)ch >= 0x20) o += ch; }
  return o;
}

void sendJson(const String& s) { server.send(200, "application/json", s); }

bool setupAuth() {
  if (server.authenticate(OTA_USER, otaPass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

void sendRestarting(const String& msg) {
  server.send(200, "text/html", "<!doctype html><meta name=viewport content='width=device-width'>"
              "<meta http-equiv=refresh content='20;url=/'><body style='font-family:system-ui;font-size:20px;padding:16px'>"
              + msg + " The fan page comes back in 20 s.");
  delay(500);
}

void apiStatus() {
  webSeen = true;
  const char* st = lidMove == LID_OPENING ? "opening" : lidMove == LID_CLOSING ? "closing" : "stopped";
  const char* wait = (fanSpeed > 0 && fanOut == 0) ? (flipAt ? "reverse" : "lid") : "";
  char buf[700];
  snprintf(buf, sizeof(buf),
           "{\"state\":\"%s\",\"pos\":%.1f,\"travel\":%lu,\"over\":%lu,\"invert\":%s,"
           "\"fan_mode\":%u,\"fan_control\":%s,\"fan\":%d,\"fan_out\":%d,\"dir\":\"%s\",\"fan_src\":\"%s\",\"fan_wait\":\"%s\","
           "\"m_hz\":%.1f,\"knob\":%d,\"knob_mv\":%d,"
           "\"wifi_up\":%s,\"wifi_ip\":\"%s\",\"wifi_rssi\":%d,\"ap\":%s,\"ap_clients\":%d,\"fw\":\"%s\",\"wifi_ssid\":\"",
           st, currentPos(), (unsigned long)cfg.travelMs, (unsigned long)cfg.overMs, cfg.invert ? "true" : "false",
           (unsigned)cfg.fanMode, fanEsp() ? "true" : "false", fanSpeed, fanOut, fanForward ? "fwd" : "rev",
           fanFromKnob ? "knob" : "phone", wait, mHz, knobPct, (int)knobMv,
           staUp ? "true" : "false", staUp ? WiFi.localIP().toString().c_str() : "", staUp ? WiFi.RSSI() : 0,
           apOn ? "true" : "false", apOn ? WiFi.softAPgetStationNum() : 0, FW_BUILD);
  sendJson(String(buf) + jsonEsc(staSsid) + "\"}");
}

void apiLid() {
  String c = server.arg("cmd");
  uint32_t ms = server.hasArg("ms") ? server.arg("ms").toInt() : 500;
  if (c == "open") lidOpen();
  else if (c == "close") lidClose();
  else if (c == "stop") lidStop("web");
  else if (c == "jog_open") lidJog(+1, ms);
  else if (c == "jog_close") lidJog(-1, ms);
  else if (c == "set_closed") { lidStop("set closed"); lidPos = 0; savePos(0); }
  else { server.send(400, "text/plain", "cmd = open | close | stop | jog_open | jog_close | set_closed"); return; }
  apiStatus();
}

void apiSettings() {
  if (server.hasArg("travel")) cfg.travelMs = constrain(server.arg("travel").toInt(), 1000, 60000);
  if (server.hasArg("over"))   cfg.overMs   = constrain(server.arg("over").toInt(), 0, 3000);
  if (server.hasArg("ramp"))   cfg.rampMs   = constrain(server.arg("ramp").toInt(), 0, 2000);
  if (server.hasArg("invert")) cfg.invert   = server.arg("invert") == "1";
  saveSettings();
  apiStatus();
}

void apiFan() {                       // speed 0-100, dir fwd | rev; a phone (or hub) request
  setFan(server.arg("speed").toInt(), server.arg("dir") != "rev", false);
  apiStatus();
}

void apiWifi() {                      // POST ssid + pass = save and join, forget = hotspot only
  if (server.hasArg("forget")) { staSsid = ""; staPass = ""; }
  else {
    String ss = server.arg("ssid"), pw = server.arg("pass");
    ss.trim();                        // the iPhone keyboard likes to add a trailing space
    if (!ss.length() || ss.length() > 32 || (pw.length() && (pw.length() < 8 || pw.length() > 63))) {
      server.send(400, "text/plain", "network name 1-32 characters, password 8-63 (empty only for an open network)");
      return;
    }
    staSsid = ss; staPass = pw;
  }
  prefs.putString("ssid", staSsid);
  prefs.putString("wpass", staPass);
  Serial.printf("router: %s\n", staSsid.length() ? staSsid.c_str() : "(forgotten)");
  wifiChanged();
  apiStatus();
}

void apiScan() {                      // first call starts a scan, then {"busy":true} until {"nets":[...]}
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

void setupPage() {
  if (!setupAuth()) return;
  webSeen = true;
  String p(SETUP_PAGE);
  p.replace("%FW%", FW_BUILD);
  for (int m = 0; m <= FAN_PHONE_KNOB; m++) p.replace("%M" + String(m) + "%", cfg.fanMode == m ? "checked" : "");
  p.replace("%KNOB%", cfg.fanMode == FAN_PHONE_KNOB ? String(knobPct) + " % (" + String((int)knobMv) + " mV)"
                                                    : String("read only in Phone + knob"));
  server.send(200, "text/html", p);
}

void apiSetup() {                     // POST fanmode and / or ap_pass, ota_pass (empty = keep); then restart
  if (!setupAuth()) return;
  String ap = server.arg("ap_pass"), op = server.arg("ota_pass");
  if ((ap.length() && (ap.length() < 8 || ap.length() > 63)) || (op.length() && (op.length() < 8 || op.length() > 63))) {
    server.send(400, "text/plain", "passwords: 8 to 63 characters");
    return;
  }
  if (server.hasArg("fanmode")) {
    int m = server.arg("fanmode").toInt();
    if (m < FAN_KNOB_DIRECT || m > FAN_PHONE_KNOB) { server.send(400, "text/plain", "fanmode 0, 1 or 2"); return; }
    prefs.putUChar("fanmode", (uint8_t)m);
    Serial.printf("fan mode -> %d\n", m);
  }
  if (ap.length()) prefs.putString("appass", ap);
  if (op.length()) prefs.putString("otapass", op);
  sendRestarting(String("Saved, restarting.") + (ap.length() ? " The MaxxFan hotspot now uses the new password." : ""));
  safeStop("setup");
  ESP.restart();
}

bool otaAuthOk = false;
String otaErr;
void webUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaErr = "";
    otaAuthOk = server.authenticate(OTA_USER, otaPass.c_str());
    if (!otaAuthOk) return;
    safeStop("update");
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
void webUpdateDone() {
  if (!otaAuthOk) return server.requestAuthentication();
  if (!otaErr.length() && !Update.isFinished()) otaErr = "no complete firmware file received";
  if (otaErr.length()) {
    if (Update.isRunning()) Update.abort();
    Serial.printf("web update failed: %s\n", otaErr.c_str());
    server.send(500, "text/plain", "Update failed: " + otaErr + "\nThe old firmware is still running.");
    return;
  }
  sendRestarting("Update received, restarting.");
  ESP.restart();
}

// ------------------------------------------------------------------ serial: o / c / s / j / J / ?
void serialLoop() {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == 'o') lidOpen();
    else if (ch == 'c') lidClose();
    else if (ch == 's') lidStop("serial");
    else if (ch == 'j') lidJog(+1, 500);
    else if (ch == 'J') lidJog(-1, 500);
    else if (ch == '?') Serial.printf("lid pos %.0f%%, travel %lu ms, invert %d | fan mode %u, %d%% %s by %s, knob %d%% (%d mV)"
                                      " | fw %s | router \"%s\" %s %s | hotspot %s\n",
                                      currentPos(), (unsigned long)cfg.travelMs, cfg.invert, (unsigned)cfg.fanMode, fanOut,
                                      zfForward ? "fwd" : "rev", fanFromKnob ? "knob" : "phone", knobPct, (int)knobMv,
                                      FW_BUILD, staSsid.c_str(), staUp ? "up" : "down",
                                      staUp ? WiFi.localIP().toString().c_str() : "", apOn ? "on 192.168.4.1" : "off");
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  loadSettings();
  lidPos = cfg.lastPos;
  if (fanEsp()) {
    // fan pins first: buffer inputs have 10k pull-downs, so until here the aqx sees EL low = STOP
    pinMode(PIN_FAN_EL, OUTPUT); digitalWrite(PIN_FAN_EL, LOW);
    pinMode(PIN_FAN_ZF, OUTPUT); digitalWrite(PIN_FAN_ZF, HIGH);
    ledcAttach(PIN_FAN_VR, FAN_PWM_HZ, 10); ledcWrite(PIN_FAN_VR, 0);
    pinMode(PIN_FAN_M, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_FAN_M), onM, RISING);
  }
  ledcAttach(PIN_LID_IN1, LID_PWM_HZ, 8);
  ledcAttach(PIN_LID_IN2, LID_PWM_HZ, 8);
  lidOutputs(0, 0);
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_BOOT, INPUT_PULLUP);
  pinMode(PIN_LID_BTN, INPUT_PULLUP);
  if (cfg.fanMode == FAN_PHONE_KNOB) {
    pinMode(PIN_DIR, INPUT);
    knobMv = analogReadMilliVolts(PIN_KNOB);
    for (int i = 0; i < 20; i++) { knobRead(); delay(2); }
    dirFwd = dirRaw = digitalRead(PIN_DIR) == HIGH;
    setFan(knobPct, dirFwd, true, false);   // the knob as found: runs the fan only if the lid is already open
    knobTaken = knobPct;
    zfForward = fanForward;
  }
  Serial.begin(115200);
  delay(300);

  esp_ota_img_states_t ota;
  fwTrial = esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota) == ESP_OK && ota == ESP_OTA_IMG_PENDING_VERIFY;
  if (fwTrial) Serial.println("new firmware on trial: open the page within 10 min to keep it");

  WiFi.persistent(false);             // the router lives in our settings, not in the Wi-Fi driver's own storage
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(false);       // wifiLoop() does the retries
  staDownMs = millis();
  if (staSsid.length()) { WiFi.mode(WIFI_STA); WiFi.setTxPower(WIFI_POWER_8_5dBm); staJoin(); }
  else apStart();

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(otaPass.c_str());
  ArduinoOTA.onStart([]() { safeStop("update"); });
  ArduinoOTA.onEnd([]() { Serial.println("update received, restarting"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("update failed (error %u), old firmware still running\n", (unsigned)e); });
  ArduinoOTA.begin();                 // also starts mDNS: maxxfan.local
  MDNS.addService("http", "tcp", 80);

  server.on("/", []() { webSeen = true; server.send_P(200, "text/html", PAGE); });
  server.on("/api/status", apiStatus);
  server.on("/api/lid", apiLid);
  server.on("/api/settings", apiSettings);
  server.on("/api/fan", apiFan);
  server.on("/api/wifi", HTTP_POST, apiWifi);
  server.on("/api/scan", apiScan);
  server.on("/api/setup", HTTP_POST, apiSetup);
  server.on("/setup", HTTP_GET, setupPage);
  server.on("/update", HTTP_GET, setupPage);
  server.on("/update", HTTP_POST, webUpdateDone, webUpdateUpload);
  server.begin();
  static const char* MODES[] = {"knob only (ESP hands off)", "phone", "phone + knob"};
  Serial.printf("MaxxFan ESP %s ready, fan mode: %s, router \"%s\"\n", FW_BUILD, MODES[cfg.fanMode],
                staSsid.length() ? staSsid.c_str() : "none: hotspot only");
}

uint32_t lastM = 0, lastBlink = 0;
void loop() {
  server.handleClient();
  ArduinoOTA.handle();
  wifiLoop();
  fwTrialLoop();
  lidLoop();
  knobLoop();
  buttonLoop();
  serialLoop();
  uint32_t now = millis();
  if (fanEsp()) {
    if (now - lastM >= 1000) { noInterrupts(); uint32_t p = mPulses; mPulses = 0; interrupts(); mHz = p * 1000.0f / (now - lastM); lastM = now; }
    fanApply();
  }
  // LED: solid while the lid moves, short blink every 2 s when idle
  bool on = (lidMove != LID_IDLE) || ((now - lastBlink) % 2000 < 60);
  digitalWrite(PIN_LED, on ? LOW : HIGH);
  if (now - lastBlink > 2000) lastBlink = now;
}
