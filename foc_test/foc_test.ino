// MaxxFan FOC board bench test: spin the fan motor on the MCF8316D from a serial console. Not the fan firmware.
//
// Board: S:\aiCAD\maxxfan_bldc\pcb\foc_board (rev A) with an ESP32-C3 SuperMini. Console: the SuperMini's USB-C,
// 115200 baud (arduino-cli monitor -p COMx -c baudrate=115200). Type "help".
//
// What it does, in order, every time it finds the chip:
//   1. takes the speed command off the SPEED pin (ALGO_DEBUG1.OVERRIDE, speed 0) so the knob can't start anything;
//   2. waits for the motor to be idle, reads the chip's EEPROM and says whether it already holds the board profile;
//   3. writes the board profile (Minuet's 57BYA54-12-01 tuning, FocMotor.cpp) into the chip's RAM, direction from the ESP.
// Nothing goes into the EEPROM unless you type "save yes". Everything else is lost when the 12 V goes off.
//
// 2026-11-10  v1: first version, for the first spin of the motor on the FOC board.

#include <Arduino.h>
#include <Wire.h>

#include <FocMotor.h>
#include <MCF8316.h>

using namespace mcf8316;

#define FOC_TEST_VERSION "v1"

// ---- pins (FOC board rev A, design.py)
const int PIN_SCL = 0;      // MCF8316D SCL (4.7k to 3V3 on the board)
const int PIN_SDA = 1;      // MCF8316D SDA (4.7k to 3V3)
const int PIN_FG = 3;       // MCF8316D FG, open drain, 10k to 3V3: 1 pulse per motor revolution (FG_DIV = pole pairs)
const int PIN_KNOB = 4;     // knob divider, also the MCF8316D SPEED pin. INPUT ONLY: never drive it.
const int PIN_LID_IN2 = 5;  // DRV8871: held low, the lid stays off in this sketch
const int PIN_LID_IN1 = 6;
const int PIN_DIR_SW = 7;   // direction-switch divider, also the MCF8316D DIR pin. Input only.
const int PIN_LED = 8;      // SuperMini blue LED, LOW = on
const int PIN_NFAULT = 10;  // MCF8316D nFAULT, open drain, 10k to 3V3: LOW = fault (the red LED D5 is on)

const float RPM_MAX_TEST = 1500.f;  // 75 % of MAX_SPEED_RPM; the fan's own top speed is Minuet's 1200
const float RPM_MIN_TEST = 200.f;   // Minuet's slowest fan speed; sensorless needs some BEMF

const MotorProfile &MOTOR = STEPPERONLINE_57BYA54_12_01;
MCF8316 chip;

// ---- state
bool connected = false;
bool escTookOver = false;     // false after "knob": the panel drives the chip
bool ccw = false;
float targetRpm = 0;
bool watch = true;
uint32_t lastProbe = 0, lastPoll = 0, lastPrint = 0, lastGood = 0;
AlgorithmState state = AlgorithmState::MOTOR_IDLE;
uint32_t gdFault = 0, ctFault = 0;
float vm = NAN, busA = NAN, phaseA = NAN, chipRpm = NAN;
Config eepromCfg{};
bool haveEeprom = false;

volatile uint32_t fgPulses = 0;
void IRAM_ATTR onFg() { fgPulses = fgPulses + 1; }
uint32_t fgLastMs = 0;
float fgHz = 0;

// ---- helpers

bool ok(Status s, const char *what) {
  if (s == Status::OK) return true;
  Serial.printf("  ! %s failed: %s\n", what, status_name(s));
  return false;
}

const char *stateName(AlgorithmState s) { return algorithm_state_name(s); }

bool motorIdle() { return state == AlgorithmState::MOTOR_IDLE; }

uint32_t knobMv() { return analogReadMilliVolts(PIN_KNOB); }

Config runtimeConfig() { return esp_direction_config(board_config(MOTOR, chip.address()), ccw); }

bool sendSpeed() {
  return ok(chip.write_speed_command(true, rpm_to_speed_fraction(targetRpm)), "speed command");
}

// Poll ALGORITHM_STATE until the motor is idle (or timeout). Keeps the speed at 0 meanwhile.
bool waitIdle(uint32_t timeoutMs) {
  uint32_t t0 = millis();
  while (millis() - t0 < timeoutMs) {
    AlgorithmState s;
    if (chip.read_state(&s) == Status::OK) {
      state = s;
      if (s == AlgorithmState::MOTOR_IDLE || s == AlgorithmState::MOTOR_FAULT) return true;
    }
    delay(100);
  }
  return false;
}

void printProfileSummary(const char *label, const Config &c) {
  Serial.printf("  %s: addr 0x%02x, speed from %s, dir from %s, %s, MAX_SPEED %u, R/L/Ke %u/%u/%u, Kp/Ki %u/%u, "
                "ILIMIT %u, buck %s\n",
                label, c.get(I2C_TARGET_ADDR),
                c.get(SPEED_MODE) == SpeedMode::SPEED_PIN_ANALOG ? "SPEED pin (analog)"
                    : c.get(SPEED_MODE) == SpeedMode::DIGITAL_SPEED_CTRL ? "I2C" : "SPEED pin (PWM/freq)",
                c.get(DIR_INPUT) == DirInput::DIR_PIN || c.get(DIR_INPUT) == DirInput::DIR_PIN2 ? "DIR pin" : "I2C",
                c.get(DEV_MODE) == DeviceMode::STANDBY ? "standby" : "SLEEP",
                c.get(MAX_SPEED), c.get(MOTOR_RES), c.get(MOTOR_IND), c.get(MOTOR_BEMF_CONST), c.get(SPD_LOOP_KP),
                c.get(SPD_LOOP_KI), unsigned(c.get(ILIMIT)), c.get(BUCK_DIS) ? "off" : "ON");
}

// ---- connect: take over, snapshot the EEPROM, load the profile into RAM

bool readEeprom() {
  if (!ok(chip.load_eeprom(), "EEPROM read")) return false;
  if (!ok(chip.read_config(&eepromCfg), "config read")) return false;
  haveEeprom = true;
  return true;
}

// Load the test's setup (profile + ESP direction) into the chip's RAM. Until this succeeds, "rpm" is refused: the RAM
// may hold anything (a new chip's factory setup, or the EEPROM after an EEPROM read).
bool applyRuntime() {
  escTookOver = false;
  Config want = runtimeConfig();
  if (!ok(chip.write_config(want), "config write")) return false;
  Config got;
  if (!ok(chip.read_config(&got), "config read-back")) return false;
  escTookOver = true;  // the writes were CRC-checked; a read-back difference is reported, not fatal
  if (!got.equals_ignoring_config_register_parity(want)) {
    Serial.println("  ! the chip's RAM reads back differently from what was written (written -> read):");
    print_config_diff(Serial, want, got);
  }
  return true;
}

void onConnect() {
  Serial.printf("\nMCF8316D answered at 0x%02x.\n", chip.address());
  targetRpm = 0;
  ok(chip.write_speed_command(true, 0), "take speed from the knob");
  if (!waitIdle(15000)) Serial.printf("  motor still not idle (%s) after 15 s\n", stateName(state));

  if (readEeprom()) {
    printProfileSummary("EEPROM now", eepromCfg);
    Config board = board_config(MOTOR, chip.address());
    if (eepromCfg.equals_ignoring_config_register_parity(board)) {
      Serial.println("  EEPROM already holds the board profile (limp home: knob + switch).");
    } else {
      Serial.println("  EEPROM is not the board profile (expected on a new chip). It is left alone; \"save yes\" "
                     "writes it once the motor runs well.");
    }
  }
  if (applyRuntime()) {
    Serial.printf("  Loaded %s profile into RAM, speed and direction from the ESP.\n", MOTOR.name);
  }
  ok(chip.clear_faults(), "clear faults");
  sendSpeed();
  Serial.println("Ready. Try: rpm 300   (stop: \"stop\" or just Enter)\n");
}

bool probe() {
  uint32_t v;
  return chip.read_register(Register::DEVICE_CONFIG1, &v) == Status::OK;
}

// ---- status

void poll() {
  AlgorithmState s;
  Status st = chip.read_state(&s);
  if (st != Status::OK) {
    if (millis() - lastGood > 2000) {
      connected = false;
      Serial.printf("\n!! Lost the MCF8316D (%s). If the motor is still running, switch off the 12 V.\n",
                    status_name(st));
    }
    return;
  }
  lastGood = millis();
  if (s != state) {
    Serial.printf("[%7.1fs] state %s -> %s\n", millis() / 1000.f, stateName(state), stateName(s));
    state = s;
  }
  uint32_t g = 0, c = 0;
  if (chip.read_faults(&g, &c) == Status::OK && (g != gdFault || c != ctFault)) {
    gdFault = g;
    ctFault = c;
    String t = fault_text(g, c);
    Serial.printf("[%7.1fs] faults: %s\n", millis() / 1000.f, t.length() ? t.c_str() : "none");
  }
  float hz;
  if (chip.read_speed_electrical_hz(max_speed_field(MOTOR), &hz) == Status::OK) chipRpm = electrical_hz_to_rpm(hz, MOTOR);
  chip.read_vm(&vm);
  chip.read_bus_current(&busA);
  chip.read_phase_peak_current(&phaseA);
}

void printStatus() {
  String f = fault_text(gdFault, ctFault);
  Serial.printf("[%7.1fs] %-22s set %4.0f rpm %s | chip %5.0f rpm  FG %5.1f Hz = %4.0f rpm | VM %5.2f V  bus %5.2f A  "
                "phase %4.2f A | knob %4lu mV  sw %d | %s%s\n",
                millis() / 1000.f, stateName(state), targetRpm, ccw ? "ccw" : "cw ", chipRpm, fgHz, fgHz * 60.f, vm,
                busA, phaseA, (unsigned long) knobMv(), digitalRead(PIN_DIR_SW), digitalRead(PIN_NFAULT) ? "" : "nFAULT ",
                f.length() ? f.c_str() : "ok");
}

// ---- commands

void help() {
  Serial.println(
      "\nFOC board bench test " FOC_TEST_VERSION ". Commands:\n"
      "  rpm N        run at N rpm (200-1500; Minuet's fan speeds are 200-1200). rpm 0 = stop\n"
      "  speed N      Minuet's fan speed N (1-10 = 200 300 400 ... 1000 1200 rpm)\n"
      "  stop         (or an empty line) speed 0: the motor recirculates to a stop, up to 5 s with the blade\n"
      "  dir cw|ccw   direction (brakes, then restarts the other way if it's running)\n"
      "  s            status now;  watch on|off  one status line a second while it runs (default on)\n"
      "  faults / clear   show / clear the chip's faults\n"
      "  dump [eeprom]    every config field in the chip's RAM (or the EEPROM snapshot)\n"
      "  diff         RAM vs the board profile\n"
      "  knob         hand speed + direction to the panel (limp-home test, RAM only); \"esp\" takes it back\n"
      "  mpet         measure the motor (R, L, Ke, inertia): spins it. Reports only, changes nothing\n"
      "  save yes     write the board profile into the chip's EEPROM (limp home). Only when it differs\n"
      "  rd 0xADDR    raw register read;  find  look for the chip at every I2C address;  i2c KHZ  bus speed\n");
}

void cmdRpm(float rpm) {
  if (!connected) { Serial.println("  no chip"); return; }
  if (!escTookOver) { Serial.println("  the panel has the motor (\"knob\"); type \"esp\" first"); return; }
  if (rpm > 0 && (ctFault || gdFault)) {
    Serial.println("  the chip reports a fault; \"clear\" first (and look at why)");
    return;
  }
  if (rpm > 0 && rpm < RPM_MIN_TEST) {
    Serial.printf("  raised to %.0f rpm, the slowest it runs sensorless\n", RPM_MIN_TEST);
    rpm = RPM_MIN_TEST;
  }
  if (rpm > RPM_MAX_TEST) {
    Serial.printf("  capped at %.0f rpm in this test\n", RPM_MAX_TEST);
    rpm = RPM_MAX_TEST;
  }
  targetRpm = rpm;
  if (sendSpeed()) Serial.printf("  speed -> %.0f rpm\n", targetRpm);
}

void cmdDir(bool wantCcw) {
  if (!connected) { Serial.println("  no chip"); return; }
  if (!escTookOver) { Serial.println("  the panel's switch has the direction (\"knob\"); type \"esp\" first"); return; }
  ccw = wantCcw;
  Config want = runtimeConfig();
  RegisterValue<Register::PERI_CONFIG1> v = want.at<Register::PERI_CONFIG1>();
  v.value = discard_config_register_parity(v.value);
  if (ok(chip.write(v), "direction")) Serial.printf("  direction -> %s\n", ccw ? "ccw" : "cw");
}

void cmdDump(bool eeprom) {
  if (eeprom) {
    if (!haveEeprom) { Serial.println("  no EEPROM snapshot yet"); return; }
    Serial.println("EEPROM snapshot (read when the chip was found):");
    print_config(Serial, eepromCfg);
    return;
  }
  Config c;
  if (ok(chip.read_config(&c), "config read")) {
    Serial.println("Chip RAM (config shadow):");
    print_config(Serial, c);
  }
}

void cmdDiff() {
  Config c;
  if (!ok(chip.read_config(&c), "config read")) return;
  Config want = escTookOver ? runtimeConfig() : board_config(MOTOR, chip.address());
  Serial.println("RAM vs the board profile (RAM -> profile):");
  int n = print_config_diff(Serial, c, want);
  Serial.printf("  %d field(s) differ\n", n);
}

void cmdKnob() {
  if (!connected) { Serial.println("  no chip"); return; }
  uint32_t mv = knobMv();
  Serial.printf("  knob reads %lu mV (the motor runs above ~150 mV)%s\n", (unsigned long) mv,
                mv > 150 ? ": IT WILL START NOW" : "");
  escTookOver = false;
  targetRpm = 0;
  Config board = board_config(MOTOR, chip.address());
  if (!ok(chip.write_config(board), "config write")) return;
  if (!ok(chip.write_speed_command(false, 0), "hand speed to the SPEED pin")) return;
  Serial.println("  The panel has the motor: knob = speed (to ~1500 rpm at the top), switch = direction. \"esp\" "
                 "takes it back.");
}

void cmdEsp() {
  if (!connected) { Serial.println("  no chip"); return; }
  targetRpm = 0;
  if (!ok(chip.write_speed_command(true, 0), "take speed")) return;
  if (applyRuntime()) Serial.println("  The ESP has the motor again (speed 0).");
}

void cmdSave(const String &arg) {
  if (!connected) { Serial.println("  no chip"); return; }
  if (arg != "yes") {
    Serial.println("  \"save yes\" writes the board profile into the chip's EEPROM (about 20 000 writes in its life;"
                   " this only writes if it differs). Motor must be stopped.");
    return;
  }
  if (targetRpm > 0 || !motorIdle()) { Serial.println("  stop the motor first"); return; }
  escTookOver = false;  // the chip's RAM is about to hold the EEPROM's content
  saveBoardProfile();
  // Whatever happened, put the test's setup back in RAM (speed stays 0).
  if (applyRuntime()) {
    ok(chip.write_speed_command(true, 0), "take speed");
  } else {
    Serial.println("  ! couldn't reload the test setup; \"esp\" retries it. Running is blocked until then.");
  }
}

void saveBoardProfile() {
  Config board = board_config(MOTOR, chip.address());
  if (!readEeprom()) return;
  if (eepromCfg.equals_ignoring_config_register_parity(board)) {
    Serial.println("  EEPROM already holds the board profile; nothing written.");
    return;
  }
  Serial.println("  EEPROM -> board profile, these fields change:");
  print_config_diff(Serial, eepromCfg, board);
  if (!ok(chip.write_config(board), "config write")) return;
  Serial.println("  writing EEPROM...");
  if (!ok(chip.save_eeprom(), "EEPROM write")) return;
  if (!readEeprom()) return;
  if (eepromCfg.equals_ignoring_config_register_parity(board)) {
    Serial.println("  EEPROM written and read back OK. Limp home is set: with the ESP out, the knob runs the fan.");
  } else {
    Serial.println("  ! EEPROM read-back differs:");
    print_config_diff(Serial, board, eepromCfg);
  }
}

void cmdMpet() {
  if (!connected || !escTookOver) { Serial.println("  needs the chip, under ESP control"); return; }
  if (targetRpm > 0 || !motorIdle()) { Serial.println("  stop the motor first"); return; }
  Serial.println("  MPET: the motor will tick, then spin up for a few seconds. Keep clear of the blade.");
  ok(chip.clear_faults(), "clear faults");
  if (!ok(chip.start_mpet(false), "start MPET")) return;
  uint32_t t0 = millis();
  bool seenRunning = false;
  while (millis() - t0 < 60000) {
    AlgorithmState s;
    if (chip.read_state(&s) == Status::OK) {
      if (s != state) {
        Serial.printf("    %s\n", stateName(s));
        state = s;
      }
      if (is_mpet_running(s)) seenRunning = true;
      if (millis() - t0 > 500 && !is_mpet_running(s)) break;
    }
    delay(50);
  }
  RegisterValue<Register::ALGO_STATUS_MPET> st;
  RegisterValue<Register::MTR_PARAMS> mp;
  RegisterValue<Register::SPEED_PI> pi;
  if (chip.read(&st) == Status::OK) {
    Serial.printf("  done%s. status R %d L %d Ke %d mech %d, PWM freq code %u\n", seenRunning ? "" : " (never saw it run)",
                  st.get(MPET_R_STATUS), st.get(MPET_L_STATUS), st.get(MPET_KE_STATUS), st.get(MPET_MECH_STATUS),
                  st.get(MPET_PWM_FREQ));
  }
  if (chip.read(&mp) == Status::OK) {
    Serial.printf("  measured MOTOR_RES %u (profile %u), MOTOR_IND %u (%u), MOTOR_BEMF_CONST %u (%u)\n",
                  mp.get(MPET_MOTOR_R), MOTOR.motor_res, mp.get(MPET_MOTOR_L), MOTOR.motor_ind,
                  mp.get(MPET_MOTOR_BEMF_CONST), MOTOR.motor_bemf_const);
  }
  if (chip.read(&pi) == Status::OK) {
    Serial.printf("  speed loop from MPET: KP %u KI %u (SPEED_PI raw; the profile's are %u / %u in config units)\n",
                  pi.get(SPEED_PI_LOOP_KP), pi.get(SPEED_PI_LOOP_KI), MOTOR.spd_loop_kp, MOTOR.spd_loop_ki);
  }
  ok(chip.clear_mpet(), "clear MPET flags");
  uint32_t g, c;
  if (chip.read_faults(&g, &c) == Status::OK && (g || c)) Serial.printf("  faults: %s\n", fault_text(g, c).c_str());
}

void cmdFind() {
  uint8_t was = chip.address();
  Serial.println("  trying every address with a CRC-checked register read...");
  int found = 0;
  for (int a = 0; a < 128; a++) {
    chip.set_address(uint8_t(a));
    uint32_t v;
    if (chip.read_register(Register::DEVICE_CONFIG1, &v) == Status::OK) {
      Serial.printf("  0x%02x answers (DEVICE_CONFIG1 0x%08lx, I2C_TARGET_ADDR field %lu)\n", a, (unsigned long) v,
                    (unsigned long) ((v >> 20) & 0x7f));
      if (!found) was = uint8_t(a);
      found++;
    }
  }
  chip.set_address(was);
  Serial.printf("  %d found; using 0x%02x\n", found, was);
}

void handle(String line) {
  line.trim();
  String cmd = line, arg;
  int sp = line.indexOf(' ');
  if (sp > 0) {
    cmd = line.substring(0, sp);
    arg = line.substring(sp + 1);
    arg.trim();
  }
  cmd.toLowerCase();
  arg.toLowerCase();
  if (cmd == "" || cmd == "stop" || cmd == "0") {
    if (connected && escTookOver) cmdRpm(0);
  } else if (cmd == "help" || cmd == "?") help();
  else if (cmd == "rpm") cmdRpm(arg.toFloat());
  else if (cmd == "speed") {
    int n = arg.toInt();
    if (n >= 1 && n <= 10) cmdRpm(MOTOR.speed_table_rpm[n - 1]);
    else Serial.println("  speed 1-10");
  } else if (cmd == "dir") {
    if (arg == "cw") cmdDir(false);
    else if (arg == "ccw") cmdDir(true);
    else Serial.println("  dir cw | dir ccw");
  } else if (cmd == "s" || cmd == "status") {
    if (connected) { poll(); printStatus(); } else Serial.println("  no chip");
  } else if (cmd == "watch") watch = (arg != "off");
  else if (cmd == "faults") {
    uint32_t g, c;
    if (ok(chip.read_faults(&g, &c), "fault read"))
      Serial.printf("  gate driver 0x%08lx, controller 0x%08lx: %s\n", (unsigned long) g, (unsigned long) c,
                    (g || c) ? fault_text(g, c).c_str() : "none");
  } else if (cmd == "clear") {
    if (ok(chip.clear_faults(), "clear faults")) Serial.println("  faults cleared");
    gdFault = ctFault = 0;
  } else if (cmd == "dump") cmdDump(arg == "eeprom");
  else if (cmd == "diff") cmdDiff();
  else if (cmd == "knob") cmdKnob();
  else if (cmd == "esp") cmdEsp();
  else if (cmd == "save") cmdSave(arg);
  else if (cmd == "mpet") cmdMpet();
  else if (cmd == "rd") {
    uint32_t addr = strtoul(arg.c_str(), nullptr, 16), v;
    if (ok(chip.read_register(Register(addr), &v), "read")) Serial.printf("  0x%03lx = 0x%08lx\n", (unsigned long) addr, (unsigned long) v);
  } else if (cmd == "find") cmdFind();
  else if (cmd == "i2c") {
    long khz = arg.toInt();
    if (khz >= 10 && khz <= 400) { Wire.setClock(khz * 1000); Serial.printf("  I2C %ld kHz\n", khz); }
    else Serial.println("  i2c 10-400");
  } else Serial.println("  ? (help)");
}

// ---- main

void setup() {
  pinMode(PIN_LID_IN1, OUTPUT);
  pinMode(PIN_LID_IN2, OUTPUT);
  digitalWrite(PIN_LID_IN1, LOW);
  digitalWrite(PIN_LID_IN2, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);
  pinMode(PIN_KNOB, INPUT);
  analogSetPinAttenuation(PIN_KNOB, ADC_11db);
  pinMode(PIN_DIR_SW, INPUT);
  pinMode(PIN_NFAULT, INPUT);
  pinMode(PIN_FG, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_FG), onFg, FALLING);

  Serial.begin(115200);
  uint32_t t0 = millis();
  while (!Serial && millis() - t0 < 3000) delay(10);

  Wire.begin(PIN_SDA, PIN_SCL, 100000);
  Wire.setTimeOut(50);
  chip.begin(Wire, MCF8316::DEFAULT_ADDRESS);
  help();
  Serial.println("Looking for the MCF8316D (needs the 12 V on)...");
}

String rx;
bool rxLastCR = false;

void loop() {
  // A line ends at CR, LF or CR LF (one line, not two: an empty line means "stop").
  while (Serial.available()) {
    char ch = char(Serial.read());
    if (ch == '\n' && rxLastCR) {
      rxLastCR = false;
      continue;
    }
    rxLastCR = (ch == '\r');
    if (ch == '\r' || ch == '\n') {
      handle(rx);
      rx = "";
    } else if (rx.length() < 80) {
      rx += ch;
    }
  }

  uint32_t now = millis();
  if (now - fgLastMs >= 1000) {
    noInterrupts();
    uint32_t p = fgPulses;
    fgPulses = 0;
    interrupts();
    fgHz = p * 1000.f / float(now - fgLastMs);
    fgLastMs = now;
  }

  if (!connected) {
    digitalWrite(PIN_LED, HIGH);
    if (now - lastProbe >= 1000) {
      lastProbe = now;
      if (probe()) {
        connected = true;
        lastGood = now;
        onConnect();
      } else if (now > 10000 && (now / 1000) % 15 == 0) {
        Serial.printf("  no answer at 0x%02x yet (12 V on? \"find\" tries every address)\n", chip.address());
      }
    }
    return;
  }

  digitalWrite(PIN_LED, LOW);
  if (now - lastPoll >= 250) {
    lastPoll = now;
    poll();
  }
  bool running = targetRpm > 0 || !motorIdle() || fgHz > 0;
  if (connected && watch && running && now - lastPrint >= 1000) {
    lastPrint = now;
    printStatus();
  }
}
