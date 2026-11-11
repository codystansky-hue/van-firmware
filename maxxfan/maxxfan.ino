// maxxfan.ino -- MaxxFan BLDC retrofit controller, ESP32-C3 SuperMini (HW-466AB)
// v1 (2026-10-03): lid control through the DRV8871 (+ phone web page, BOOT button, serial).
// v2 (2026-10-09, never flashed): joins the van router (Starlink) saved on the page, MaxxFan hotspot only as a fallback;
//   firmware updates over Wi-Fi with automatic rollback.
// v3 (2026-10-09): fan wiring picked on the setup page (one firmware for every build stage); knob + direction switch read
//   by the ESP so the knob and the phone both work, whichever was touched last wins; optional panel lid button; passwords
//   kept on the ESP, not in git; a fan reversal no longer blocks the loop. M (speed pulses) moved G0 -> G10.
// v3.1 (2026-10-11): the Wi-Fi, update and password code moved to libraries/vannet (shared with the hub). Same behaviour;
//   the "restarting" page now says "The page comes back in 20 s".
// v4 (2026-11-10): a fourth fan mode (3), the FOC board (S:\aiCAD\maxxfan_bldc\pcb\foc_board): the MCF8316D runs the motor (sensorless
//   FOC, Minuet's profile, libraries/mcf8316) and the ESP sets speed and direction over I2C in the chip's RAM. Picked by
//   itself when the chip answers at start-up. Fan page: motor line (state, rpm, current, faults), run-at-rpm box, top
//   speed, swap direction. Setup page: write the knob fallback (limp home) into the chip's EEPROM, by hand, once.
//   The other fan modes are unchanged.
// v4.1 (2026-11-10): the knob runs the lid. Its click, held 3 s, closes the lid; just past the click is vent (lid open,
//   fan off); beyond that, fan speed (Phone + knob and FOC board). New phone page: one screen with the lid, a 10-step
//   speed slider (Minuet's speeds, scaled to the top speed), exhaust / intake and off; calibration, fan settings, motor
//   details and Wi-Fi under Settings; plain-language motor faults. Fixes: a fan request while the lid was closing never
//   reopened it; reversing the lid mid-move timed the new move from the old start position.
//
// Arduino core "esp32" by Espressif 3.x (3.3.12). Board settings are in sketch.yaml: ESP32C3 Dev Module, USB CDC On Boot
// Enabled, Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA)". Build with --libraries libraries (the repo's folder,
// for vannet and mcf8316). Wiring, flashing and use: README.md next to this file.

#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <VanNet.h>
#include <MCF8316.h>
#include <FocMotor.h>

// ------------------------------------------------------------------ configuration
#define FW_VERSION "v4.1"
const char* FW_BUILD = FW_VERSION " " __DATE__ " " __TIME__;

void safeStop(const char* why);
const vannet::Config NET = {
  .hostname       = "maxxfan",        // http://maxxfan.local on the van router (mDNS)
  .apSsid         = "MaxxFan",        // the ESP's own hotspot (fallback when the van router is unreachable)
  .setupUser      = "maxxfan",        // setup page login; its password is also the PC-upload password
  .defaultApPass  = "maxxfan123",     // first-boot passwords only: change them on the setup page, which keeps them on
  .defaultOtaPass = "maxxfan-ota",    //   the ESP (not in git)
  .prefsNs        = "maxxfan",
  .safeStop       = safeStop,
};

// fan wiring, picked on the setup page (applied at restart). FAN_FOC is also picked by itself, and saved, when the FOC
// board's motor chip answers at start-up.
enum FanMode : uint8_t {
  FAN_KNOB_DIRECT = 0,                // knob harness (H11) in the aqx plug; the ESP leaves the fan pins alone
  FAN_PHONE       = 1,                // 74AHCT125 buffer fitted, ESP ribbon in the aqx plug: the phone runs the fan
  FAN_PHONE_KNOB  = 2,                // + knob harness in the panel port: knob and phone, whichever was touched last
  FAN_FOC         = 3                 // FOC board (MCF8316D over I2C): knob and phone, whichever was touched last
};
// where the knob is in its travel (knobLoop): at its click, the vent band, or the fan part
enum KnobZone : uint8_t { KZ_OFF, KZ_VENT, KZ_FAN };

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
const int PIN_DIR     = 7;            // G7 <- 10k/20k divider <- direction switch, 5 V = forward [2, 3]
// FOC board [3]: the dividers are on the board, same values as the panel port, so the knob calibration holds
const int PIN_FOC_SCL    = 0;         // G0 -> MCF8316D SCL (4.7k pull-up on the board)
const int PIN_FOC_SDA    = 1;         // G1 <-> MCF8316D SDA (4.7k pull-up)
const int PIN_FOC_FG     = 3;         // G3 <- MCF8316D FG, one pulse per motor revolution (10k pull-up)
const int PIN_FOC_KNOB   = 4;         // G4 <- knob divider, which is also the MCF8316D SPEED pin: INPUT ONLY, never driven
const int PIN_FOC_NFAULT = 10;        // G10 <- MCF8316D nFAULT, LOW = fault (the board's red LED)
int pinKnob = PIN_KNOB;               // G0, or G4 on the FOC board

const uint32_t KNOB_OFF_MV  = 120;    // below this the knob is at its click = off
const uint32_t KNOB_VENT_MV = 420;    // click .. this = vent: lid open, fan off (about the first 15 % of the travel)
const uint32_t KNOB_FULL_MV = 2150;   // knob fully up (the divider gives ~2270 mV; the setup page shows the reading)
const uint32_t KNOB_HYST_MV = 25;     // hysteresis at the click / vent / fan edges
const uint32_t KNOB_CLOSE_MS = 3000;  // the knob sits at its click this long: the lid closes
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
  uint32_t topRpm;       // FOC board: rpm at 100 % (1 % = FOC_MIN_RPM)
  bool     fanSwap;      // FOC board: swap if "exhaust" blows in
} cfg;
// the van router and the passwords are in the same namespace, kept by vannet

void loadSettings() {
  prefs.begin("maxxfan", false);
  cfg.travelMs = prefs.getUInt("travel", 12000);
  cfg.overMs   = prefs.getUInt("over", 600);
  cfg.rampMs   = prefs.getUInt("ramp", 300);
  cfg.invert   = prefs.getBool("invert", false);
  cfg.lastPos  = prefs.getFloat("pos", -1.0f);
  cfg.fanMode  = prefs.getUChar("fanmode", FAN_KNOB_DIRECT);
  if (cfg.fanMode > FAN_FOC) cfg.fanMode = FAN_KNOB_DIRECT;
  cfg.topRpm   = constrain(prefs.getUInt("toprpm", 1200), 400, 1500);
  cfg.fanSwap  = prefs.getBool("fanswap", false);
}
void saveSettings() {
  prefs.putUInt("travel", cfg.travelMs);
  prefs.putUInt("over", cfg.overMs);
  prefs.putUInt("ramp", cfg.rampMs);
  prefs.putBool("invert", cfg.invert);
  prefs.putUInt("toprpm", cfg.topRpm);
  prefs.putBool("fanswap", cfg.fanSwap);
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

// currentPos(), not lidPos: when reversing mid-move, lidPos still holds where the move started (v4.1 fix)
void lidOpen()  { float p = currentPos(); if (p < 0) p = 0; lidStart(+1, (uint32_t)((100.0f - p) / 100.0f * cfg.travelMs), true); }
void lidClose() {
  if (!fanFromKnob) fanSpeed = 0;       // closing cancels a phone fan request (a knob that is up stays up)
  float p = currentPos(); if (p < 0) p = 100;
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
int  knobPct = 0, knobTaken = 0;      // fan % the knob asks for now / when the fan was last set (0 at the click and vent)
// The knob's travel (v4.1): its click = off (parked there 3 s: the lid closes), then a vent band (lid open, fan off), then
// fan speed 1-100 %. Only a knob movement acts; a phone command wins until the knob moves again.
KnobZone knobZone = KZ_OFF, knobZoneTaken = KZ_OFF;
uint32_t knobCloseAt = 0;             // the knob was turned to its click: close the lid at this time (0 = no)
bool dirFwd = true, dirRaw = true;    // direction switch: debounced / raw
uint32_t knobT = 0, dirChangeMs = 0;

void IRAM_ATTR onM() { mPulses = mPulses + 1; }   // single writer (this ISR); C++20 deprecates ++ on volatile

// ------------------------------------------------------------------ fan on the FOC board (MCF8316D), fan mode 3
// The chip runs the motor: sensorless FOC with Minuet's profile for the 57BYA54-12-01 (libraries/mcf8316/FocMotor.cpp).
// The ESP takes the speed (ALGO_DEBUG1 override) and the direction (PERI_CONFIG1) over I2C, in the chip's RAM only, so a
// chip power cycle hands both back to the knob + switch. The knob fallback (limp home, ESP dead) needs the board profile
// in the chip's EEPROM: written by hand from the setup page, once the motor runs well (focSaveEeprom).
// Speed: 1 % = FOC_MIN_RPM, 100 % = the top speed set on the page (Minuet's top is 1200 rpm).
const mcf8316::MotorProfile& FOC_MOTOR = mcf8316::STEPPERONLINE_57BYA54_12_01;
const float FOC_MIN_RPM = 200;        // Minuet's slowest; sensorless needs some back-EMF
mcf8316::MCF8316 foc;
bool focUp = false;                   // the chip answers and has the ESP's setup in RAM
uint32_t focProbeMs = 0, focPollMs = 0, focCheckMs = 0, focGoodMs = 0;
mcf8316::AlgorithmState focState = mcf8316::AlgorithmState::MOTOR_IDLE;
uint32_t focGd = 0, focCt = 0;        // gate driver / controller fault bits
float focRpm = 0, focVm = 0, focBusA = 0, focPhaseA = 0;
float focSentRpm = -1;                // what the chip was last told (-1 = tell it)
int   focSentCcw = -1;
int8_t focEeprom = -1;                // -1 unknown, 0 not the board profile, 1 the board profile (limp home set)
bool focFoundAtBoot = false;

float pctToRpm(int s) { return s <= 0 ? 0 : FOC_MIN_RPM + (cfg.topRpm - FOC_MIN_RPM) * (s - 1) / 99.0f; }
int rpmToPct(float rpm) {
  if (rpm <= 0) return 0;
  return constrain(1 + (int)lroundf((rpm - FOC_MIN_RPM) * 99.0f / (cfg.topRpm - FOC_MIN_RPM)), 1, 100);
}
// Forward (exhaust) = the chip's clockwise (OUTA-B-C), which is what the switch gives on the DIR pin with the ESP dead
// (switch forward = DIR high, SLLSFX9A 7.3.2.1). The swap box changes the ESP only; swapping two motor wires fixes both.
bool focCcw(bool forward) { return forward ? cfg.fanSwap : !cfg.fanSwap; }

void focWireBegin() {
  Wire.begin(PIN_FOC_SDA, PIN_FOC_SCL, 100000);
  Wire.setTimeOut(20);
  foc.begin(Wire);
}

// At start-up, in any fan mode: is there an MCF8316D on G0 / G1? (A CRC-checked register read; nothing else answers.)
bool focProbeAtBoot() {
  focWireBegin();
  for (int i = 0; i < 3; i++) {
    uint32_t v;
    if (foc.read_register(mcf8316::Register::DEVICE_CONFIG1, &v) == mcf8316::Status::OK) return true;
    delay(30);
  }
  Wire.end();                         // not the FOC board: give G0 / G1 back
  return false;
}

// Chip found (or back): take the speed off the knob first, note what its EEPROM holds (only when the motor is idle:
// reading the EEPROM reloads the chip's RAM), then load the ESP's setup into its RAM.
bool focSetupChip() {
  using namespace mcf8316;
  focSentRpm = -1;
  focSentCcw = -1;
  if (foc.write_speed_command(true, 0) != Status::OK) return false;
  AlgorithmState s;
  if (foc.read_state(&s) == Status::OK && s == AlgorithmState::MOTOR_IDLE) {
    Config e;
    if (foc.load_eeprom() == Status::OK && foc.read_config(&e) == Status::OK)
      focEeprom = e.equals_ignoring_config_register_parity(board_config(FOC_MOTOR, foc.address())) ? 1 : 0;
  }
  bool ccw = focCcw(zfForward);
  if (foc.write_config(esp_direction_config(board_config(FOC_MOTOR, foc.address()), ccw)) != Status::OK) return false;
  if (foc.write_speed_command(true, 0) != Status::OK) return false;
  foc.clear_faults();
  focGd = focCt = 0;
  focSentRpm = 0;
  focSentCcw = ccw;
  return true;
}

void focLoop() {
  using namespace mcf8316;
  uint32_t now = millis();
  if (!focUp) {
    if (now - focProbeMs < 1000) return;
    focProbeMs = now;
    uint32_t v;
    if (foc.read_register(Register::DEVICE_CONFIG1, &v) != Status::OK) return;
    focUp = focSetupChip();
    focGoodMs = now;
    Serial.printf("FOC: motor chip %s\n", focUp ? "found and set up" : "answers but the setup failed, retrying");
    return;
  }
  if (now - focPollMs < 250) return;
  focPollMs = now;
  AlgorithmState s;
  if (foc.read_state(&s) != Status::OK) {
    if (now - focGoodMs > 2000) {
      focUp = false;
      Serial.println("FOC: lost the motor chip (12 V off?)");
    }
    return;
  }
  focGoodMs = now;
  if (s != focState) Serial.printf("FOC: %s\n", algorithm_state_name(s));
  focState = s;
  uint32_t g, c;
  if (foc.read_faults(&g, &c) == Status::OK) {
    if (g != focGd || c != focCt) Serial.printf("FOC: faults %s\n", (g || c) ? fault_text(g, c).c_str() : "cleared");
    focGd = g;
    focCt = c;
  }
  float hz;
  if (foc.read_speed_electrical_hz(max_speed_field(FOC_MOTOR), &hz) == Status::OK) focRpm = electrical_hz_to_rpm(hz, FOC_MOTOR);
  foc.read_vm(&focVm);
  foc.read_bus_current(&focBusA);
  foc.read_phase_peak_current(&focPhaseA);
  if (now - focCheckMs >= 2000) {     // still ours? A chip power cycle drops the override and reloads its EEPROM
    focCheckMs = now;
    RegisterValue<Register::ALGO_DEBUG1> d;
    if (foc.read(&d) == Status::OK && !d.get(DIGITAL_SPEED_OVERRIDE)) {
      Serial.println("FOC: the motor chip restarted, setting it up again");
      focUp = focSetupChip();
    }
  }
}

// Tell the chip the speed / direction fanApply wants, when it changes.
void focApply(int s, bool forward) {
  using namespace mcf8316;
  if (!focUp) return;
  int ccw = focCcw(forward);
  if (ccw != focSentCcw) {
    Config want = esp_direction_config(board_config(FOC_MOTOR, foc.address()), ccw);
    RegisterValue<Register::PERI_CONFIG1> v = want.at<Register::PERI_CONFIG1>();
    v.value = discard_config_register_parity(v.value);
    if (foc.write(v) == Status::OK) focSentCcw = ccw;
  }
  float rpm = pctToRpm(s);
  if (rpm != focSentRpm && foc.write_speed_command(true, rpm_to_speed_fraction(rpm)) == Status::OK) focSentRpm = rpm;
}

// Setup page button: write the board profile into the chip's EEPROM (knob + switch run the fan with the ESP dead).
// Only with the motor stopped, only if it differs: the EEPROM is good for ~20 000 writes.
String focSaveEeprom() {
  using namespace mcf8316;
  if (cfg.fanMode != FAN_FOC || !focUp) return "The motor chip isn't answering.";
  if (fanOut > 0 || focState != AlgorithmState::MOTOR_IDLE)
    return "Turn the fan off and wait until it has stopped, then try again.";
  Config board = board_config(FOC_MOTOR, foc.address());
  Config e;
  String msg;
  if (foc.load_eeprom() != Status::OK || foc.read_config(&e) != Status::OK) msg = "Couldn't read the chip's EEPROM.";
  else if (e.equals_ignoring_config_register_parity(board)) msg = "The chip already has the knob fallback; nothing written.";
  else if (foc.write_config(board) != Status::OK) msg = "Couldn't write the profile to the chip.";
  else if (foc.save_eeprom() != Status::OK) msg = "The chip didn't finish the EEPROM write.";
  else if (foc.load_eeprom() != Status::OK || foc.read_config(&e) != Status::OK) msg = "Written, but couldn't read it back.";
  else if (!e.equals_ignoring_config_register_parity(board)) msg = "Written, but it reads back different.";
  else msg = "Saved. With the ESP out, the knob and switch now run the fan (to about 1500 rpm at the top).";
  focUp = focSetupChip();             // the ESP's setup back in RAM, fan stopped (re-checks the EEPROM)
  if (!focUp) msg += " The ESP couldn't take the motor back yet; it keeps trying.";
  Serial.printf("FOC: %s\n", msg.c_str());
  return msg;
}

void lidEnsureOpen() {                // open the lid unless it's open or already opening (reverses a close)
  if (lidMove == LID_OPENING) return;
  if (lidMove == LID_IDLE && lidPos >= 95.0f) return;
  lidOpen();
}

void setFan(int speed, bool forward, bool fromKnob, bool mayOpenLid = true) {
  if (!fanEsp()) return;
  fanSpeed = constrain(speed, 0, 100); fanForward = forward; fanFromKnob = fromKnob;
  if (!fromKnob) { knobTaken = knobPct; knobZoneTaken = knobZone; knobCloseAt = 0; }   // the knob has to move again
  if (cfg.fanMode == FAN_FOC && focUp && (focGd || focCt)) { foc.clear_faults(); }   // a new request retries a fault
  if (mayOpenLid && fanSpeed > 0) lidEnsureOpen();
}

bool fanSpinning() { return fanOut > 0 || mHz > 1 || (cfg.fanMode == FAN_FOC && focUp && fabsf(focRpm) > 30); }

void fanApply() {
  if (!fanEsp()) return;
  uint32_t now = millis();
  int s = (lidMove == LID_IDLE && lidPos >= 95.0f) ? fanSpeed : 0;     // interlock
  if (fanForward != zfForward) {                                       // never reverse at speed
    if (!flipAt) flipAt = now + (fanSpinning() ? FLIP_WAIT_MS : 1);
    if ((int32_t)(now - flipAt) < 0) s = 0;
    else { zfForward = fanForward; flipAt = 0; }
  } else flipAt = 0;
  fanOut = s;
  if (cfg.fanMode == FAN_FOC) { focApply(s, zfForward); return; }
  digitalWrite(PIN_FAN_ZF, zfForward ? HIGH : LOW);
  digitalWrite(PIN_FAN_EL, s > 0 ? HIGH : LOW);
  ledcWrite(PIN_FAN_VR, (uint32_t)(1023UL * s / 100));
}

// Which part of its travel the knob is in, with hysteresis at each edge so it doesn't flicker there.
KnobZone knobZoneFor(float mv, KnobZone prev) {
  float e1 = (float)KNOB_OFF_MV + (prev == KZ_OFF ? (float)KNOB_HYST_MV : -(float)KNOB_HYST_MV);
  float e2 = (float)KNOB_VENT_MV + (prev == KZ_FAN ? -(float)KNOB_HYST_MV : (float)KNOB_HYST_MV);
  return mv < e1 ? KZ_OFF : mv < e2 ? KZ_VENT : KZ_FAN;
}
int knobToPct(float mv) {             // fan % in the fan part of the travel
  return constrain(1 + (int)lroundf(99.0f * (mv - KNOB_VENT_MV) / (KNOB_FULL_MV - KNOB_VENT_MV)), 1, 100);
}
void knobRead() {
  knobMv += 0.25f * ((float)analogReadMilliVolts(pinKnob) - knobMv);
  knobZone = knobZoneFor(knobMv, knobZone);
  knobPct = knobZone == KZ_FAN ? knobToPct(knobMv) : 0;
}
bool hasKnob() { return cfg.fanMode == FAN_PHONE_KNOB || cfg.fanMode == FAN_FOC; }
const char* knobZoneName(KnobZone z) { return z == KZ_FAN ? "fan" : z == KZ_VENT ? "vent" : "off"; }

void knobLoop() {
  if (!hasKnob()) return;
  uint32_t now = millis();
  if (now - knobT < 20) return;
  knobT = now;
  knobRead();
  bool d = digitalRead(PIN_DIR) == HIGH;
  if (d != dirRaw) { dirRaw = d; dirChangeMs = now; }
  bool flipped = (dirRaw != dirFwd && now - dirChangeMs > 50);
  if (flipped) dirFwd = dirRaw;
  bool zoneMoved = knobZone != knobZoneTaken;
  int step = abs(knobPct - knobTaken);
  bool deliberate = zoneMoved || flipped || step >= 5;      // a real turn: may move the lid
  bool follow = fanFromKnob && step >= 2;
  if (deliberate || follow) {
    if (!fanFromKnob) Serial.println("fan: the knob took over");
    knobTaken = knobPct;
    knobZoneTaken = knobZone;
    setFan(knobPct, dirFwd, true, deliberate);              // fan part: may open the lid; click / vent: fan off
    if (zoneMoved) {
      knobCloseAt = 0;
      if (knobZone == KZ_VENT) { Serial.println("knob: vent, lid open, fan off"); lidEnsureOpen(); }
      if (knobZone == KZ_OFF) knobCloseAt = (now + KNOB_CLOSE_MS) | 1;   // never 0
    }
  }
  if (knobCloseAt && (int32_t)(now - knobCloseAt) >= 0) {   // still at the click, nobody else touched it: close
    knobCloseAt = 0;
    if (fanFromKnob && knobZone == KZ_OFF && !(lidMove == LID_IDLE && lidPos == 0.0f)) {
      Serial.println("knob: at its click, closing the lid");
      lidClose();
    }
  }
}

// ------------------------------------------------------------------ buttons: BOOT or panel lid button, tap = open / close / stop
uint32_t btnDownMs = 0; bool btnWas = false;
void buttonLoop() {
  bool down = digitalRead(PIN_BOOT) == LOW || digitalRead(PIN_LID_BTN) == LOW;
  if (down && !btnWas) btnDownMs = millis();
  if (!down && btnWas && millis() - btnDownMs > 40) {
    knobCloseAt = 0;                    // a button press overrides a pending knob close
    if (lidMove != LID_IDLE) lidStop("button");
    else if (lidPos >= 50.0f) lidClose();
    else lidOpen();
  }
  btnWas = down;
}

// ------------------------------------------------------------------ Wi-Fi and firmware updates: libraries/vannet
// The phone stays on the van router: http://maxxfan.local; the MaxxFan hotspot is the fallback. Updates: a .bin uploaded
// on http://maxxfan.local/setup or a PC on the same network (espota), both with the setup password, with automatic
// rollback. Either one calls safeStop() first: loop() is blocked while it runs.
void safeStop(const char* why) {      // before an update or a restart
  if (lidMove != LID_IDLE) { lidStop(why); delay(150); }
  lidOutputs(0, 0); brakeUntil = 0;   // coast
  if (fanEsp()) { fanSpeed = 0; fanApply(); }
  Serial.printf("%s: lid and fan stopped\n", why);
}

// ------------------------------------------------------------------ web pages
WebServer server(80);

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover"><title>MaxxFan</title>
<meta name="apple-mobile-web-app-capable" content="yes"><meta name="apple-mobile-web-app-title" content="MaxxFan">
<style>
:root{--bg:#f2f5f4;--card:#fff;--fg:#17201d;--mut:#5d6b66;--rule:#d6dfdb;--acc:#0d6b5a;--acc2:#dcefe9;--on:#fff;--bad:#b3261e;--bad2:#f9dedb;color-scheme:light}
@media(prefers-color-scheme:dark){:root{--bg:#0f1513;--card:#17201d;--fg:#e4ece8;--mut:#93a39d;--rule:#2a3632;--acc:#52c2a8;--acc2:#173a32;--on:#0f1513;--bad:#ff8a80;--bad2:#3d1c19;color-scheme:dark}}
*{box-sizing:border-box}[hidden]{display:none!important}
body{margin:0;background:var(--bg);color:var(--fg);font:16px/1.4 -apple-system,system-ui,"Segoe UI",Roboto,sans-serif;
padding:max(12px,env(safe-area-inset-top)) max(14px,env(safe-area-inset-right)) max(28px,env(safe-area-inset-bottom)) max(14px,env(safe-area-inset-left))}
.w{max-width:460px;margin:0 auto;display:grid;gap:14px}.v{display:grid;gap:14px;min-width:0}
header{display:flex;align-items:center;justify-content:space-between;gap:10px}
h1{font-size:22px;margin:0;font-weight:650}
h2{font-size:12px;margin:0;font-weight:600;letter-spacing:.08em;text-transform:uppercase;color:var(--mut)}
.pill{font-size:13px;padding:4px 10px;border-radius:999px;background:var(--card);border:1px solid var(--rule);color:var(--mut);white-space:nowrap;overflow:hidden;text-overflow:ellipsis;max-width:62%}
.pill.ok::before{content:"";display:inline-block;width:7px;height:7px;border-radius:50%;background:var(--acc);margin-right:6px;vertical-align:1px}
.card{background:var(--card);border:1px solid var(--rule);border-radius:14px;padding:14px;display:grid;gap:12px}
.hd{display:flex;align-items:baseline;justify-content:space-between;gap:10px}
.val{font-size:17px;font-weight:600;text-align:right;font-variant-numeric:tabular-nums}
.sub{font-size:13px;color:var(--mut)}
.bar{height:10px;border-radius:5px;background:var(--acc2);overflow:hidden}
.bar>i{display:block;height:100%;width:0;background:var(--acc)}
.bar.unk>i{width:100%;background:repeating-linear-gradient(45deg,var(--acc2) 0 6px,var(--rule) 6px 12px)}
.row{display:flex;gap:10px}.row>*{flex:1;min-width:0}
button{font:inherit;font-size:18px;font-weight:600;padding:14px;border-radius:12px;border:1px solid var(--rule);background:var(--bg);color:var(--fg);cursor:pointer;touch-action:manipulation}
button.pri{background:var(--acc);border-color:var(--acc);color:var(--on)}
button:focus-visible,input:focus-visible,select:focus-visible,a:focus-visible{outline:2px solid var(--acc);outline-offset:2px}
input[type=range]{width:100%;accent-color:var(--acc);height:34px;margin:0}
.ticks{display:flex;justify-content:space-between;font-size:12px;color:var(--mut);padding:0 5px;margin-top:-8px;font-variant-numeric:tabular-nums}
.seg{display:flex;border:1px solid var(--rule);border-radius:12px;overflow:hidden}
.seg button{flex:1;border:0;border-radius:0;font-size:16px;padding:12px;background:var(--card)}
.seg button+button{border-left:1px solid var(--rule)}
.seg button.on{background:var(--acc2);color:var(--fg)}
.alrt{background:var(--bad2);border:1px solid var(--bad);border-radius:12px;padding:12px;display:grid;gap:10px;font-size:15px}
.alrt b{color:var(--bad)}.alrt button{font-size:15px;padding:10px}
.foot{display:flex;justify-content:space-between;gap:10px;font-size:13px;color:var(--mut);padding:0 2px}
a{color:var(--acc)}
.sm{font-size:15px;padding:11px}
input[type=number],input[type=text],input[type=password],select{font:inherit;font-size:16px;padding:9px;border-radius:8px;border:1px solid var(--rule);background:var(--bg);color:var(--fg);width:100%}
label.ck{display:flex;gap:10px;align-items:center;font-size:15px}label.ck input{width:20px;height:20px;accent-color:var(--acc)}
.f{display:grid;grid-template-rows:1fr auto;gap:4px;font-size:14px;color:var(--mut)}
dl{display:grid;grid-template-columns:auto 1fr;gap:5px 14px;margin:0;font-size:14px}dt{color:var(--mut)}dd{margin:0;font-variant-numeric:tabular-nums;overflow-wrap:anywhere}
</style></head><body><div class="w">
<header><h1>MaxxFan</h1><span id="net" class="pill">...</span></header>

<div id="vhome" class="v">
<section class="card">
<div class="hd"><h2>Lid</h2><span id="lidv" class="val">...</span></div>
<div id="lidbar" class="bar"><i></i></div>
<div class="row"><button id="lidb" class="pri" onclick="lidT()">Open</button><button onclick="lc('stop')">Stop</button></div>
</section>
<section class="card">
<div class="hd"><h2>Fan</h2><span id="fanv" class="val">...</span></div>
<div id="fanctl" class="v" style="gap:12px;width:100%">
<input type="range" id="spd" min="0" max="10" step="1" value="0" aria-label="Fan speed, 0 is off">
<div class="ticks"><span>off</span><span>1</span><span>2</span><span>3</span><span>4</span><span>5</span><span>6</span><span>7</span><span>8</span><span>9</span><span>10</span></div>
<div class="seg"><button id="dx" onclick="dirn('fwd')">Exhaust</button><button id="di" onclick="dirn('rev')">Intake</button></div>
<button onclick="fan(0)">Off</button>
</div>
<div id="fsub" class="sub"></div>
</section>
<div id="alrt" class="alrt" hidden><div id="altx"></div><button id="altb" onclick="clr()" hidden>Clear the motor fault</button></div>
<div class="foot"><span id="foot"></span><a href="#settings">Settings &rsaquo;</a></div>
</div>

<div id="vset" class="v" hidden>
<div class="foot"><a href="#">&lsaquo; Back</a><span id="fw"></span></div>
<section class="card"><h2>Lid</h2>
<div class="sub">Close it fully (nudge if needed) and press <b>Set closed</b>. Then time a full Open with a stopwatch and enter it as full travel.</div>
<div class="row"><button class="sm" onclick="lc('jog_open')">Nudge open</button><button class="sm" onclick="lc('jog_close')">Nudge close</button></div>
<button class="sm" onclick="lc('set_closed')">Set closed (position = 0 %)</button>
<div class="row"><label class="f">full travel, ms<input id="tr" type="number" min="1000" max="60000" step="100"></label><label class="f">extra at the ends, ms<input id="ov" type="number" min="0" max="3000" step="50"></label></div>
<label class="ck"><input type="checkbox" id="inv"> Swap the lid's direction (if Open closes it)</label>
<button class="sm pri" onclick="saveLid()">Save lid settings</button>
</section>
<section class="card" id="sfan"><h2>Fan</h2>
<div id="sfoc" class="v" style="gap:12px;width:100%">
<div class="row"><label class="f">run at, rpm<input id="rpm" type="number" min="200" max="1500" step="50" value="400"></label><label class="f">top speed, rpm<input id="toprpm" type="number" min="400" max="1500" step="50"></label></div>
<div class="row"><button class="sm" onclick="runRpm('fwd')">Run exhaust</button><button class="sm" onclick="runRpm('rev')">Run intake</button></div>
<label class="ck"><input type="checkbox" id="swp"> Swap the fan's direction (if exhaust blows in; swapping two motor wires in J2 is better: it fixes the knob fallback too)</label>
<button class="sm pri" onclick="saveFan()">Save fan settings</button>
</div>
<div class="sub" id="knobtx"></div>
</section>
<section class="card" id="smot"><h2>Motor</h2><dl id="mkv"></dl>
<div class="sub">The knob fallback (the knob runs the fan with the ESP dead) is saved into the motor chip from <a href="/setup">setup</a>.</div></section>
<section class="card"><h2>Wi-Fi</h2><div id="wnet" class="sub">...</div>
<div class="sub">Save the van router (Starlink) and your phone can stay on it: bookmark <b>http://maxxfan.local</b>. The MaxxFan hotspot then turns off, and comes back by itself when the router is off for 20 s.</div>
<select id="nets" onchange="$('ss').value=this.value" hidden></select>
<input id="ss" type="text" placeholder="network name" autocapitalize="off" autocorrect="off" spellcheck="false">
<input id="pw" type="password" placeholder="password" autocapitalize="off" autocorrect="off">
<div class="row"><button class="sm" onclick="scan()">Scan</button><button class="sm pri" onclick="join()">Save and join</button></div>
<button class="sm" onclick="forget()">Forget the router (hotspot only)</button><div id="wm" class="sub"></div>
</section>
<div class="foot"><a href="/setup">Setup: passwords, firmware update, fan wiring, knob fallback</a></div>
</div>
</div>
<script>
const $=id=>document.getElementById(id);
let R=null,first=true,hold=0,armForget=0;
async function g(u){try{let r=await fetch(u);if(r.ok)show(await r.json())}catch(e){}}
function lc(c){g('/api/lid?cmd='+c)}
function lidAct(){if(!R)return'open';return R.state=='opening'?'close':R.state=='closing'?'open':(R.pos>=50?'close':'open')}
function lidT(){lc(lidAct())}
function curDir(){return R&&R.dir=='rev'?'rev':'fwd'}
function fan(n){hold=Date.now()+2500;$('spd').value=n;
 if(!n)return g('/api/fan?speed=0&dir='+curDir());
 g(R&&R.steps?'/api/fan?rpm='+R.steps[n-1]+'&dir='+curDir():'/api/fan?speed='+n*10+'&dir='+curDir())}
function dirn(d){g('/api/fan?speed='+(R?R.fan:0)+'&dir='+d)}
function runRpm(d){if($('rpm').value>0)g('/api/fan?rpm='+$('rpm').value+'&dir='+d)}
function clr(){g('/api/foc?cmd=clear')}
function saveLid(){g('/api/settings?travel='+$('tr').value+'&over='+$('ov').value+'&invert='+($('inv').checked?1:0))}
function saveFan(){g('/api/settings?top_rpm='+$('toprpm').value+'&fan_swap='+($('swp').checked?1:0))}
function stepOf(r){if(!r.fan)return 0;
 if(r.steps){let b=1,bd=1e9;r.steps.forEach((s,i)=>{let d=Math.abs(s-r.foc.want_rpm);if(d<bd){bd=d;b=i+1}});return b}
 return Math.min(10,Math.max(1,Math.round(r.fan/10)))}
const ZONE={off:'at its click',vent:'at vent',fan:'up'};
const WHY=[[/NO_MTR/,'a motor wire is loose'],[/IPD_/,'it could not find the rotor at start-up'],[/MTR_LCK|ABN_SPEED|ABN_BEMF/,'it lost track of the rotor'],[/LOCK_ILIMIT/,'too much current'],[/UNDER_VOLTAGE/,'the 12 V dropped under 8 V'],[/OVER_VOLTAGE|OVP/,'the supply went over 18 V'],[/OTW|OTS/,'the motor chip is hot']];
function why(t){let w=WHY.filter(x=>x[0].test(t)).map(x=>x[1]);return w.length?w.join(', ')+'.':''}
$('spd').oninput=()=>{hold=Date.now()+2500;let n=+$('spd').value;$('fanv').textContent=n?'speed '+n+(R&&R.steps?' · '+R.steps[n-1]+' rpm':''):'off'};
$('spd').onchange=()=>fan(+$('spd').value);
function show(r){R=r;
 let n=$('net');n.textContent=r.wifi_up?r.wifi_ssid:r.ap?'MaxxFan hotspot':'no Wi-Fi';n.className='pill'+(r.wifi_up?' ok':'');
 let p=r.pos,lv=r.state=='opening'?'opening':r.state=='closing'?'closing':p<0?'position unknown':p>=95?'open':p<=0?'closed':Math.round(p)+'% open';
 $('lidv').textContent=lv;$('lidbar').className='bar'+(p<0?' unk':'');$('lidbar').firstChild.style.width=(p<0?0:p)+'%';
 $('lidb').textContent=lidAct()=='open'?'Open':'Close';
 let s=stepOf(r),f=r.foc,parts=[];
 $('fanctl').hidden=r.fan_mode==0;
 if(r.fan_mode==0){$('fanv').textContent='knob';parts.push('The knob runs the fan directly (fan wiring: knob only).')}
 else{if(Date.now()>hold){$('spd').value=s;$('fanv').textContent=r.fan?'speed '+s+(f?' · '+f.want_rpm+' rpm':''):'off'}
  $('dx').className=r.dir=='fwd'?'on':'';$('di').className=r.dir=='rev'?'on':'';
  if(r.fan)parts.push(r.dir=='fwd'?'exhaust':'intake');
  if(r.fan_wait=='lid')parts.push('waits for the lid to open');else if(r.fan_wait=='reverse')parts.push('reversing');
  if(r.fan_mode>=2)parts.push('set by '+r.fan_src+(r.knob_zone?', knob '+ZONE[r.knob_zone]+(r.knob_zone=='fan'?' ('+r.knob+'%)':''):''))}
 $('fsub').textContent=parts.join(' · ');
 let a='';if(f&&!f.up)a='<b>Motor chip not answering.</b> Is the 12 V on?';else if(f&&f.fault)a='<b>Motor stopped:</b> '+(why(f.fault)||'a fault.')+'<div class="sub">'+f.fault.replace(/\|/g,', ')+'</div>';
 $('altx').innerHTML=a;$('alrt').hidden=!a;$('altb').hidden=!(f&&f.up&&f.fault);
 $('foot').textContent=f&&f.up?(f.state=='IDLE'?'motor stopped':Math.round(f.rpm)+' rpm')+' · '+f.vm+' V':'firmware '+r.fw.split(' ')[0];
 $('fw').textContent='firmware '+r.fw;
 $('sfan').hidden=r.fan_mode==0;$('sfoc').hidden=!f;$('smot').hidden=!f;
 $('knobtx').textContent=r.knob_zone?'Knob: '+ZONE[r.knob_zone]+' ('+r.knob_mv+' mV). Turned to its click for 3 s, it closes the lid. Just past the click is vent: lid open, fan off. Beyond that, fan speed.':'';
 if(f)$('mkv').innerHTML='<dt>state</dt><dd>'+f.state+'</dd><dt>speed</dt><dd>'+Math.round(f.rpm)+' rpm (FG '+Math.round(r.m_hz*60)+')</dd><dt>asked for</dt><dd>'+f.sent_rpm+' rpm</dd><dt>supply</dt><dd>'+f.vm+' V, '+f.bus_a+' A</dd><dt>phase peak</dt><dd>'+f.phase_a+' A</dd><dt>faults</dt><dd>'+(f.fault||'none')+'</dd><dt>knob fallback</dt><dd>'+(f.eeprom=='board'?'saved in the motor chip':f.eeprom=='other'?'not saved yet':'unknown')+'</dd>';
 let w=!r.wifi_ssid?'No router saved.':r.wifi_up?'On '+r.wifi_ssid+' ('+r.wifi_ip+', '+r.wifi_rssi+' dBm).':'Trying '+r.wifi_ssid+' ...';
 w+=r.ap?' Hotspot on.':' Hotspot off.';if(r.wifi_up&&r.ap&&r.ap_clients)w+=' Switch your phone to '+r.wifi_ssid+' and open http://maxxfan.local';
 $('wnet').textContent=w;
 if(first){first=false;$('tr').value=r.travel;$('ov').value=r.over;$('inv').checked=r.invert;$('ss').value=r.wifi_ssid;
  if(f){$('toprpm').value=r.top_rpm;$('swp').checked=r.fan_swap}}}
async function wifi(p){let r=await fetch('/api/wifi',{method:'POST',body:new URLSearchParams(p)});$('wm').textContent=r.ok?'':await r.text();g('/api/status')}
function join(){if($('ss').value)wifi({ssid:$('ss').value,pass:$('pw').value})}
function forget(){if(Date.now()<armForget){armForget=0;wifi({forget:1})}else{armForget=Date.now()+4000;$('wm').textContent='Tap again to forget the router. The fan will only be on its own MaxxFan hotspot.'}}
async function scan(){$('wm').textContent='Scanning ...';for(let i=0;i<15;i++){try{let r=await (await fetch('/api/scan')).json();
 if(r.nets){let s=$('nets');s.innerHTML='<option value="">pick a network ('+r.nets.length+' found)</option>';
  r.nets.forEach(x=>{let o=document.createElement('option');o.value=o.textContent=x;s.appendChild(o)});s.hidden=false;$('wm').textContent='';return}}catch(e){}
 await new Promise(z=>setTimeout(z,1000))}$('wm').textContent='Scan failed: type the name.'}
function route(){let s=location.hash=='#settings';$('vhome').hidden=s;$('vset').hidden=!s;window.scrollTo(0,0)}
window.onhashchange=route;route();g('/api/status');setInterval(()=>g('/api/status'),1000);
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
<label><input type="radio" name="fanmode" value="3" %M3%> <b>FOC board</b>: the MCF8316D board runs the motor; knob and
phone, whichever you touched last. Picked by itself when the board's motor chip answers at start-up.</label>
<small>Knob now: %KNOB%</small>
<button>Save and restart</button></form>
%FOC%
<h2>Passwords</h2>
<form method="POST" action="/api/setup">
MaxxFan hotspot <input class="w" type="password" name="ap_pass" placeholder="leave empty to keep" autocomplete="new-password">
setup + updates (user maxxfan) <input class="w" type="password" name="ota_pass" placeholder="leave empty to keep" autocomplete="new-password">
<small>8 to 63 characters. Kept on the ESP, through firmware updates.</small>
<button>Save and restart</button></form></body></html>)HTML";

void sendJson(const String& s) { server.send(200, "application/json", s); }
String focJson();

void apiStatus() {
  vannet::pageSeen();
  const char* st = lidMove == LID_OPENING ? "opening" : lidMove == LID_CLOSING ? "closing" : "stopped";
  const char* wait = (fanSpeed > 0 && fanOut == 0) ? (flipAt ? "reverse" : "lid") : "";
  char buf[500];
  snprintf(buf, sizeof(buf),
           "{\"state\":\"%s\",\"pos\":%.1f,\"travel\":%lu,\"over\":%lu,\"invert\":%s,"
           "\"fan_mode\":%u,\"fan_control\":%s,\"fan\":%d,\"fan_out\":%d,\"dir\":\"%s\",\"fan_src\":\"%s\",\"fan_wait\":\"%s\","
           "\"m_hz\":%.1f,\"knob\":%d,\"knob_mv\":%d,\"knob_zone\":\"%s\",\"fw\":\"%s\",",
           st, currentPos(), (unsigned long)cfg.travelMs, (unsigned long)cfg.overMs, cfg.invert ? "true" : "false",
           (unsigned)cfg.fanMode, fanEsp() ? "true" : "false", fanSpeed, fanOut, fanForward ? "fwd" : "rev",
           fanFromKnob ? "knob" : "phone", wait, mHz, knobPct, (int)knobMv, hasKnob() ? knobZoneName(knobZone) : "",
           FW_BUILD);
  String j(buf);
  if (cfg.fanMode == FAN_FOC) j += focJson();
  sendJson(j + vannet::statusFields() + "}");
}

// The phone page's 10 fan speeds: Minuet's table (200 ... 1000, 1200 rpm) scaled so speed 10 = the top speed.
uint32_t speedStepRpm(int n) {
  uint32_t r = (uint32_t)lroundf(FOC_MOTOR.speed_table_rpm[n - 1] * (float)cfg.topRpm / 1200.0f / 10.0f) * 10;
  return constrain(r, (uint32_t)FOC_MIN_RPM, cfg.topRpm);
}

String focJson() {                    // "foc":{...},"top_rpm":..,"fan_swap":..,"steps":[..],  (FOC board only)
  String steps = "\"steps\":[";
  for (int n = 1; n <= 10; n++) steps += String(speedStepRpm(n)) + (n < 10 ? "," : "],");
  char buf[420];
  String f = mcf8316::fault_text(focGd, focCt);
  snprintf(buf, sizeof(buf),
           "\"foc\":{\"up\":%s,\"state\":\"%s\",\"rpm\":%.0f,\"want_rpm\":%.0f,\"sent_rpm\":%.0f,\"vm\":%.1f,\"bus_a\":%.2f,"
           "\"phase_a\":%.2f,\"fault\":\"%s\",\"eeprom\":\"%s\"},\"top_rpm\":%lu,\"fan_swap\":%s,",
           focUp ? "true" : "false", mcf8316::algorithm_state_name(focState), focRpm, pctToRpm(fanSpeed),
           focSentRpm < 0 ? 0.0f : focSentRpm, focVm, focBusA, focPhaseA, vannet::jsonEsc(f).c_str(),
           focEeprom < 0 ? "unknown" : focEeprom ? "board" : "other", (unsigned long)cfg.topRpm,
           cfg.fanSwap ? "true" : "false");
  return String(buf) + steps;
}

void apiLid() {
  String c = server.arg("cmd");
  uint32_t ms = server.hasArg("ms") ? server.arg("ms").toInt() : 500;
  knobCloseAt = 0;                      // the phone overrides a pending knob close
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
  if (server.hasArg("top_rpm")) cfg.topRpm  = constrain(server.arg("top_rpm").toInt(), 400, 1500);
  if (server.hasArg("fan_swap")) cfg.fanSwap = server.arg("fan_swap") == "1";
  saveSettings();
  apiStatus();
}

void apiFan() {                       // speed 0-100 (or, FOC board, rpm=200-1500), dir fwd | rev; a phone (or hub) request
  int speed = server.arg("speed").toInt();
  if (cfg.fanMode == FAN_FOC && server.hasArg("rpm")) speed = rpmToPct(constrain(server.arg("rpm").toFloat(), 0.0f, (float)cfg.topRpm));
  setFan(speed, server.arg("dir") != "rev", false);
  apiStatus();
}

void apiFoc() {                       // FOC board: cmd=clear (the chip's faults)
  if (server.arg("cmd") == "clear" && cfg.fanMode == FAN_FOC && focUp) {
    foc.clear_faults();
    focGd = focCt = 0;
  }
  apiStatus();
}

void apiFocSave() {                   // POST, setup login: the knob fallback into the chip's EEPROM
  if (!vannet::setupAuth()) return;
  vannet::pageSeen();
  String msg = focSaveEeprom();
  server.send(200, "text/html", "<!doctype html><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
              "<body style=\"font-family:system-ui,sans-serif;max-width:460px;margin:16px auto;padding:0 12px\"><p>" + msg +
              "</p><p><a href=\"/setup\">back to setup</a> &middot; <a href=\"/\">the fan</a></p></body>");
}

void setupPage() {
  if (!vannet::setupAuth()) return;
  vannet::pageSeen();
  String p(SETUP_PAGE);
  p.replace("%FW%", FW_BUILD);
  for (int m = 0; m <= FAN_FOC; m++) p.replace("%M" + String(m) + "%", cfg.fanMode == m ? "checked" : "");
  p.replace("%KNOB%", hasKnob() ? String(knobPct) + " % (" + String((int)knobMv) + " mV)"
                                : String("read only in Phone + knob and FOC board"));
  String f;
  if (cfg.fanMode == FAN_FOC) {
    f = "<h2>Knob fallback (FOC board)</h2><p>Motor chip: <b>";
    f += !focUp ? "not answering" : focEeprom == 1 ? "has the knob fallback" : focEeprom == 0 ? "no knob fallback yet"
                                                   : "unknown (it was running when the ESP started)";
    f += "</b></p><form method=\"POST\" action=\"/api/foc_save\"><button>Save the knob fallback into the motor chip</button>"
         "</form><small>Once the motor runs well from the phone. Then, with the ESP dead or unplugged, the knob and switch "
         "still run the fan (to about 1500 rpm at the top; the lid only by hand). Writes the chip's EEPROM, which is good "
         "for about 20 000 writes, and only if it differs. The fan must be off and stopped.</small>";
  }
  p.replace("%FOC%", f);
  server.send(200, "text/html", p);
}

void apiSetup() {                     // POST fanmode and / or ap_pass, ota_pass (empty = keep); then restart
  if (!vannet::setupAuth()) return;
  String err = vannet::passwordError();
  if (err.length()) { server.send(400, "text/plain", err); return; }
  if (server.hasArg("fanmode")) {
    int m = server.arg("fanmode").toInt();
    if (m < FAN_KNOB_DIRECT || m > FAN_FOC) { server.send(400, "text/plain", "fanmode 0, 1, 2 or 3"); return; }
    prefs.putUChar("fanmode", (uint8_t)m);
    Serial.printf("fan mode -> %d\n", m);
  }
  bool apChanged = vannet::savePasswords();
  vannet::sendRestarting(String("Saved, restarting.") + (apChanged ? " The MaxxFan hotspot now uses the new password." : ""));
  safeStop("setup");
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
                                      FW_BUILD, vannet::routerSsid().c_str(), vannet::routerUp() ? "up" : "down",
                                      vannet::routerUp() ? WiFi.localIP().toString().c_str() : "",
                                      vannet::hotspotOn() ? "on 192.168.4.1" : "off");
    if (ch == '?' && cfg.fanMode == FAN_FOC)
      Serial.printf("FOC: chip %s, %s, %.0f rpm (sent %.0f), VM %.1f V, bus %.2f A, phase %.2f A, faults %s, EEPROM %s\n",
                    focUp ? "up" : "not answering", mcf8316::algorithm_state_name(focState), focRpm, focSentRpm, focVm,
                    focBusA, focPhaseA, (focGd || focCt) ? mcf8316::fault_text(focGd, focCt).c_str() : "none",
                    focEeprom < 0 ? "unknown" : focEeprom ? "board profile" : "not the board profile");
  }
}

// ------------------------------------------------------------------ setup / loop
void setup() {
  loadSettings();
  lidPos = cfg.lastPos;
  // The FOC board answers on G0 / G1 at power-up (its chip runs from the same 12 V as the ESP): switch to it, once.
  if (cfg.fanMode != FAN_FOC && focProbeAtBoot()) {
    cfg.fanMode = FAN_FOC;
    prefs.putUChar("fanmode", FAN_FOC);
    focFoundAtBoot = true;
  }
  if (cfg.fanMode == FAN_FOC) {
    // G4 (knob = the chip's SPEED pin) and G7 (switch = DIR) are inputs only; G3 FG and G10 nFAULT are open drain
    if (!focFoundAtBoot) focWireBegin();
    pinMode(PIN_FOC_KNOB, INPUT);
    pinMode(PIN_FOC_NFAULT, INPUT);
    pinMode(PIN_FOC_FG, INPUT);
    attachInterrupt(digitalPinToInterrupt(PIN_FOC_FG), onM, FALLING);
    pinKnob = PIN_FOC_KNOB;
  } else if (fanEsp()) {
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
  if (hasKnob()) {
    pinMode(PIN_DIR, INPUT);
    knobMv = analogReadMilliVolts(pinKnob);
    for (int i = 0; i < 20; i++) { knobRead(); delay(2); }
    dirFwd = dirRaw = digitalRead(PIN_DIR) == HIGH;
    setFan(knobPct, dirFwd, true, false);   // the knob as found: runs the fan only if the lid is already open
    knobTaken = knobPct;
    knobZoneTaken = knobZone;               // and never moves the lid at start-up
    zfForward = fanForward;
  }
  Serial.begin(115200);
  delay(300);

  vannet::begin(server, NET);         // Wi-Fi, mDNS (maxxfan.local), updates; adds /api/wifi, /api/scan, POST /update
  server.on("/", []() { vannet::pageSeen(); server.send_P(200, "text/html", PAGE); });
  server.on("/api/status", apiStatus);
  server.on("/api/lid", apiLid);
  server.on("/api/settings", apiSettings);
  server.on("/api/fan", apiFan);
  server.on("/api/foc", apiFoc);
  server.on("/api/foc_save", HTTP_POST, apiFocSave);
  server.on("/api/setup", HTTP_POST, apiSetup);
  server.on("/setup", HTTP_GET, setupPage);
  server.on("/update", HTTP_GET, setupPage);
  server.begin();
  static const char* MODES[] = {"knob only (ESP hands off)", "phone", "phone + knob", "FOC board"};
  Serial.printf("MaxxFan ESP %s ready, fan mode: %s%s, router \"%s\"\n", FW_BUILD, MODES[cfg.fanMode],
                focFoundAtBoot ? " (motor chip found at start-up)" : "",
                vannet::routerSsid().length() ? vannet::routerSsid().c_str() : "none: hotspot only");
}

uint32_t lastM = 0, lastBlink = 0;
void loop() {
  server.handleClient();
  vannet::loop();
  lidLoop();
  knobLoop();
  buttonLoop();
  serialLoop();
  uint32_t now = millis();
  if (fanEsp()) {
    if (now - lastM >= 1000) { noInterrupts(); uint32_t p = mPulses; mPulses = 0; interrupts(); mHz = p * 1000.0f / (now - lastM); lastM = now; }
    if (cfg.fanMode == FAN_FOC) focLoop();
    fanApply();
  }
  // LED: solid while the lid moves, short blink every 2 s when idle
  bool on = (lidMove != LID_IDLE) || ((now - lastBlink) % 2000 < 60);
  digitalWrite(PIN_LED, on ? LOW : HIGH);
  if (now - lastBlink > 2000) lastBlink = now;
}
