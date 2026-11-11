#include "MCF8316.h"

#include <cmath>

namespace mcf8316 {

namespace {

// 24-bit control word flags (SLLSFX9A table 7-10). MEM_SEC = MEM_PAGE = 0 for every register we use.
constexpr uint32_t CTRL_OP_READ = 0x800000;
constexpr uint32_t CTRL_CRC_EN = 0x400000;
constexpr uint32_t CTRL_DLEN_32 = 0x100000;
constexpr int READ_TRIES = 3;

uint8_t crc8(uint8_t crc, const uint8_t *data, size_t length) {
  while (length--) {
    crc ^= *data++;
    for (int i = 0; i < 8; i++) {
      crc = (crc & 0x80) ? uint8_t((crc << 1) ^ 0x07) : uint8_t(crc << 1);
    }
  }
  return crc;
}

}  // namespace

const char *status_name(Status s) {
  switch (s) {
    case Status::OK: return "OK";
    case Status::I2C: return "I2C (no answer)";
    case Status::CRC: return "CRC mismatch";
    case Status::DEVICE: return "chip did not finish";
  }
  return "?";
}

Status MCF8316::read_register(Register reg, uint32_t *value) {
  const uint32_t ctrl = CTRL_OP_READ | CTRL_CRC_EN | CTRL_DLEN_32 | uint32_t(reg);
  const uint8_t cw[3] = {uint8_t(ctrl >> 16), uint8_t(ctrl >> 8), uint8_t(ctrl)};
  const uint8_t addr_w = uint8_t(this->address_ << 1);
  const uint8_t addr_r = uint8_t(addr_w | 1);
  Status status = Status::I2C;
  for (int attempt = 0; attempt < READ_TRIES; attempt++) {
    this->wire_->beginTransmission(this->address_);
    this->wire_->write(cw, sizeof(cw));
    if (this->wire_->endTransmission(false) != 0) {  // repeated start next
      this->i2c_errors_++;
      status = Status::I2C;
      continue;
    }
    uint8_t buf[5];
    const size_t got = this->wire_->requestFrom(uint16_t(this->address_), size_t(sizeof(buf)), true);
    if (got != sizeof(buf)) {
      while (this->wire_->available()) this->wire_->read();
      this->i2c_errors_++;
      status = Status::I2C;
      continue;
    }
    for (uint8_t &b : buf) b = uint8_t(this->wire_->read());
    uint8_t crc = 0xff;
    crc = crc8(crc, &addr_w, 1);
    crc = crc8(crc, cw, sizeof(cw));
    crc = crc8(crc, &addr_r, 1);
    crc = crc8(crc, buf, 4);
    if (crc != buf[4]) {
      this->crc_errors_++;
      status = Status::CRC;
      continue;
    }
    *value = uint32_t(buf[0]) | (uint32_t(buf[1]) << 8) | (uint32_t(buf[2]) << 16) | (uint32_t(buf[3]) << 24);
    return Status::OK;
  }
  return status;
}

Status MCF8316::write_register(Register reg, uint32_t value, int tries) {
  const uint32_t ctrl = CTRL_CRC_EN | CTRL_DLEN_32 | uint32_t(reg);
  uint8_t buf[8] = {
      uint8_t(ctrl >> 16), uint8_t(ctrl >> 8), uint8_t(ctrl),
      uint8_t(value), uint8_t(value >> 8), uint8_t(value >> 16), uint8_t(value >> 24),
      0,
  };
  const uint8_t addr_w = uint8_t(this->address_ << 1);
  buf[7] = crc8(crc8(0xff, &addr_w, 1), buf, 7);
  for (int attempt = 0; attempt < tries; attempt++) {
    this->wire_->beginTransmission(this->address_);
    this->wire_->write(buf, sizeof(buf));
    if (this->wire_->endTransmission(true) == 0) return Status::OK;
    this->i2c_errors_++;
  }
  return Status::I2C;
}

Status MCF8316::read_config(Config *config) {
  for (size_t i = 0; i < Config::LENGTH; i++) {
    Status s = this->read_register(Config::index_to_register(i), &config->register_values[i].value);
    if (s != Status::OK) return s;
  }
  return Status::OK;
}

Status MCF8316::write_config(const Config &config) {
  for (size_t i = 0; i < Config::LENGTH; i++) {
    Status s = this->write_register(Config::index_to_register(i),
                                    discard_config_register_parity(config.register_values[i].value));
    if (s != Status::OK) return s;
  }
  return Status::OK;
}

Status MCF8316::load_eeprom() {
  RegisterValue<Register::ALGO_CTRL1> ctrl;
  ctrl.set(EEPROM_READ, true);
  Status s = this->write(ctrl);
  if (s != Status::OK) return s;
  delay(200);  // SLLA664: allow 200 ms for the read
  s = this->read(&ctrl);
  if (s != Status::OK) return s;
  return ctrl.value == 0 ? Status::OK : Status::DEVICE;
}

Status MCF8316::save_eeprom() {
  RegisterValue<Register::ALGO_CTRL1> ctrl;
  ctrl.set(EEPROM_WRT, true);
  ctrl.set(EEPROM_WRITE_ACCESS_KEY, 0xa5u);  // = 0x8A500000
  Status s = this->write_register(Register::ALGO_CTRL1, ctrl.value, 1);  // never repeat an EEPROM write blindly
  if (s != Status::OK) return s;
  delay(750);  // datasheet: allow 750 ms for the write
  for (int i = 0; i < 10; i++) {
    s = this->read(&ctrl);
    if (s == Status::OK && ctrl.value == 0) return Status::OK;
    delay(100);
  }
  return s != Status::OK ? s : Status::DEVICE;
}

Status MCF8316::clear_faults() {
  RegisterValue<Register::ALGO_CTRL1> ctrl;
  ctrl.set(CLR_FLT, true);
  ctrl.set(CLR_FLT_RETRY_COUNT, true);
  return this->write(ctrl);
}

Status MCF8316::read_faults(uint32_t *gate_driver, uint32_t *controller) {
  Status s = this->read_register(Register::GATE_DRIVER_FAULT_STATUS, gate_driver);
  if (s != Status::OK) return s;
  return this->read_register(Register::CONTROLLER_FAULT_STATUS, controller);
}

Status MCF8316::read_state(AlgorithmState *state) {
  RegisterValue<Register::ALGORITHM_STATE> v;
  Status s = this->read(&v);
  if (s == Status::OK) *state = v.get(ALGORITHM_STATE);
  return s;
}

Status MCF8316::write_speed_command(bool override_on, float fraction) {
  if (!(fraction > 0.f)) fraction = 0.f;  // also catches NaN
  if (fraction > 1.f) fraction = 1.f;
  RegisterValue<Register::ALGO_DEBUG1> v;  // CLOSED_LOOP_DIS / FORCE_* stay 0
  v.set(DIGITAL_SPEED_OVERRIDE, override_on);
  v.set(DIGITAL_SPEED_CTRL, unsigned(lroundf(fraction * 32767.f)));
  return this->write(v);
}

Status MCF8316::read_speed_electrical_hz(unsigned max_speed, float *hz) {
  RegisterValue<Register::SPEED_FDBK> v;
  Status s = this->read(&v);
  if (s == Status::OK) *hz = float(int32_t(v.value)) * (float(max_speed) / 6.f) / float(1ul << 27);
  return s;
}

Status MCF8316::read_vm(float *volts) {
  RegisterValue<Register::VM_VOLTAGE> v;
  Status s = this->read(&v);
  if (s == Status::OK) *volts = float(v.value) * 60.f / float(1ul << 27);
  return s;
}

Status MCF8316::read_bus_current(float *amps) {
  RegisterValue<Register::BUS_CURRENT> v;
  Status s = this->read(&v);
  if (s == Status::OK) *amps = float(int32_t(v.value)) * 10.f / float(1ul << 30);
  return s;
}

Status MCF8316::read_phase_peak_current(float *amps) {
  RegisterValue<Register::IMAG_SQR> v;
  Status s = this->read(&v);
  if (s == Status::OK) *amps = sqrtf(float(v.value) / float(1ul << 27)) * 10.f;
  return s;
}

Status MCF8316::start_mpet(bool write_shadow) {
  RegisterValue<Register::ALGO_DEBUG2> v;
  v.set(MPET_CMD, 1u);
  v.set(MPET_R, true);
  v.set(MPET_L, true);
  v.set(MPET_KE, true);
  v.set(MPET_MECH, true);
  v.set(MPET_WRITE_SHADOW, write_shadow);
  return this->write(v);
}

Status MCF8316::clear_mpet() {
  RegisterValue<Register::ALGO_DEBUG2> v;  // all MPET bits 0, so it doesn't rerun after a fault clear
  return this->write(v);
}

// ---- printing

#define VANNET_MCF_PRINT_FIELD(field) \
  out.printf("    %-38s %lu\n", #field, (unsigned long) uint32_t(config.get(field)));
#define VANNET_MCF_PRINT_REGISTER(reg) \
  out.printf("  %-16s 0x%08lx\n", #reg, (unsigned long) config.at<Register::reg>().value); \
  MCF8316_FOR_EACH_FIELD_OF_REGISTER(reg, VANNET_MCF_PRINT_FIELD)

void print_config(Print &out, const Config &config) {
  MCF8316_FOR_EACH_CONFIG_REGISTER(VANNET_MCF_PRINT_REGISTER)
  out.printf("  composite: SPD_LOOP_KP %u\n", config.get(SPD_LOOP_KP));
}

#define VANNET_MCF_DIFF_FIELD(field) \
  if (uint32_t(a.get(field)) != uint32_t(b.get(field))) { \
    out.printf("    %-38s %lu -> %lu\n", #field, (unsigned long) uint32_t(a.get(field)), \
               (unsigned long) uint32_t(b.get(field))); \
    n++; \
  }
#define VANNET_MCF_DIFF_REGISTER(reg) MCF8316_FOR_EACH_FIELD_OF_REGISTER(reg, VANNET_MCF_DIFF_FIELD)

int print_config_diff(Print &out, const Config &a, const Config &b) {
  int n = 0;
  MCF8316_FOR_EACH_CONFIG_REGISTER(VANNET_MCF_DIFF_REGISTER)
  return n;
}

String fault_text(uint32_t gate_driver, uint32_t controller) {
  String s;
  if (gate_driver) s += format_gate_driver_fault_status(gate_driver).c_str();
  if (controller) {
    if (s.length()) s += ' ';
    s += format_controller_fault_status(controller).c_str();
  }
  return s;
}

}  // namespace mcf8316
