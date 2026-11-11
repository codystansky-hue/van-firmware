// TI MCF8316D over I2C (Arduino Wire), for the MaxxFan FOC board.
//
// Every transfer is a 24-bit control word plus 32-bit data, always with CRC-8 (poly 0x07, init 0xFF over the whole
// packet, address bytes included), the protocol in SLLSFX9A section 7.6.2. Ported from brown-studios/esphome-mcf8316
// (MIT). The chip doesn't answer a bare address probe, so "is it there" is a register read with a good CRC.
//
// Config registers (0x80-0xAE) are a RAM shadow loaded from EEPROM at power-up. Writes to them take effect at once and
// are lost at power-off. save_eeprom() copies the shadow into EEPROM: about 20 000 write cycles, so never call it on
// every boot, only when the content differs (see FocMotor.h).
#pragma once

#include <Arduino.h>
#include <Wire.h>

#include "mcf8316_registers.h"

namespace mcf8316 {

enum class Status : uint8_t {
  OK = 0,
  I2C,      // no ACK / short read
  CRC,      // reply arrived but the CRC was wrong
  DEVICE,   // the chip didn't finish a command (EEPROM read / write)
};
const char *status_name(Status s);

class MCF8316 {
 public:
  static constexpr uint8_t DEFAULT_ADDRESS = 0x01;

  // Wire must already be begun (Wire.begin(SDA, SCL, 100000)).
  void begin(TwoWire &wire, uint8_t address = DEFAULT_ADDRESS) {
    this->wire_ = &wire;
    this->address_ = address;
  }
  uint8_t address() const { return this->address_; }
  void set_address(uint8_t address) { this->address_ = address; }

  // Raw access. Reads retry up to 3 times on an I2C or CRC error; writes retry on an I2C error unless tries = 1.
  Status read_register(Register reg, uint32_t *value);
  Status write_register(Register reg, uint32_t value, int tries = 3);
  template<Register reg> Status read(RegisterValue<reg> *v) { return this->read_register(reg, &v->value); }
  template<Register reg> Status write(const RegisterValue<reg> &v) { return this->write_register(reg, v.value); }

  // The 24 config shadow registers. write_config drops the parity bit (the chip computes its own).
  Status read_config(Config *config);
  Status write_config(const Config &config);

  Status load_eeprom();   // EEPROM -> shadow (~200 ms). Unlimited.
  Status save_eeprom();   // shadow -> EEPROM (~750 ms). Motor must be stopped. ~20 000 cycles: use sparingly.

  Status clear_faults();  // CLR_FLT + CLR_FLT_RETRY_COUNT
  Status read_faults(uint32_t *gate_driver, uint32_t *controller);
  Status read_state(AlgorithmState *state);

  // ALGO_DEBUG1: OVERRIDE = 1 makes DIGITAL_SPEED_CTRL the speed command whatever SPEED_MODE says (SLLSFX9A table
  // 9-16); OVERRIDE = 0 hands it back to the SPEED pin (the knob). Volatile: a power cycle clears it.
  // fraction = 0..1 of MAX_SPEED.
  Status write_speed_command(bool override_on, float fraction);

  // Live readings. Speed is the estimator's (SPEED_FDBK), in electrical Hz; max_speed = the MAX_SPEED field.
  Status read_speed_electrical_hz(unsigned max_speed, float *hz);
  Status read_vm(float *volts);
  Status read_bus_current(float *amps);
  Status read_phase_peak_current(float *amps);

  // Motor parameter extraction (spins the motor). write_shadow = copy the results into the config shadow.
  Status start_mpet(bool write_shadow);
  Status clear_mpet();

  uint32_t i2c_errors() const { return this->i2c_errors_; }
  uint32_t crc_errors() const { return this->crc_errors_; }

 private:
  TwoWire *wire_{nullptr};
  uint8_t address_{DEFAULT_ADDRESS};
  uint32_t i2c_errors_{0};
  uint32_t crc_errors_{0};
};

// Print every config field (name = value), one register per block.
void print_config(Print &out, const Config &config);
// Print the fields that differ between a and b ("NAME: a -> b"). Returns how many differ.
int print_config_diff(Print &out, const Config &a, const Config &b);
// Fault bits as names, "" when none.
String fault_text(uint32_t gate_driver, uint32_t controller);

}  // namespace mcf8316
