// The MaxxFan FOC board (S:\aiCAD\maxxfan_bldc\pcb\foc_board, rev A): its MCF8316D setup and the fan motor's profile.
//
// Two layers, matching the board's limp-home design:
//   * board_config(): what belongs in the chip's EEPROM. Speed from the SPEED pin (the knob divider, analog), direction
//     from the DIR pin (the switch divider), standby rather than sleep (I2C always up, the SPEED pin is the knob and
//     can't double as WAKE), internal buck off (FB_BK fed from the board's 5 V), external FG / nFAULT pull-ups.
//     With the ESP dead the knob and switch still run the fan.
//   * The ESP takes over at runtime without touching EEPROM: ALGO_DEBUG1.OVERRIDE for speed (MCF8316::
//     write_speed_command) and runtime_config() for direction. Both are lost when the chip loses power, so a power cycle
//     always comes back to the knob.
//
// Motor profile and tuning: brown-studios/minuet-firmware minuet/fan_driver.h (MIT), tuned by Minuet on this exact motor
// in a MaxxFan Deluxe. Their settings are carried over field by field; see FocMotor.cpp.
#pragma once

#include "mcf8316_registers.h"

namespace mcf8316 {

struct MotorProfile {
  const char *name;
  unsigned motor_poles;                      // FG_DIV = poles / 2 (pole pairs)
  int lead_angle_deg;                        // BEMF lead angle
  unsigned motor_res;                        // MPET units (MOTOR_RES)
  unsigned motor_ind;                        // MOTOR_IND
  unsigned motor_bemf_const;                 // MOTOR_BEMF_CONST
  unsigned spd_loop_kp;                      // SPD_LOOP_KP (10 bits)
  unsigned spd_loop_ki;                      // SPD_LOOP_KI (10 bits)
  IPDClockFrequency ipd_clk_freq;            // higher = quieter start "tick"; too high = IPD faults
  IPDCurrentThreshold ipd_curr_thr;          // lower = quieter tick; too low = may start backwards
  CurrentLimit ol_ilimit;                    // open-loop phase current, ~1/2 to 2/3 of ilimit
  OpenLoopAcceleration ol_acc_a1;            // electrical Hz/s
  CurrentLimit ilimit;                       // closed-loop phase current limit, < LOCK_ILIMIT
  ClosedLoopSlowAcceleration cl_slow_acc;    // while the estimator is still aligning
  ClosedLoopAcceleration cl_acc;
  ClosedLoopDeceleration cl_dec;
  uint16_t speed_table_rpm[10];              // Minuet's 10 fan speeds
};

extern const MotorProfile STEPPERONLINE_57BYA54_12_01;

// 100 % speed reference, in rpm. Many settings below (handoff, brake and resync thresholds) are a percentage of it, so
// leave it at Minuet's value. With the knob topping out at 2.27 V of the SPEED pin's 3.0 V full scale, limp home runs to
// about 76 % of this, ~1500 rpm.
constexpr float MAX_SPEED_RPM = 2000.f;

// Board limits (Minuet's figures, which fit this board: 5 A inline fuse, MCF8316D 8 A peak).
constexpr float BUS_POWER_LIMIT_W = 40.f;
constexpr CurrentLimit LOCK_ILIMIT_A = CurrentLimit::LIMIT_4_5_A;      // software lock (just above ilimit)
constexpr CurrentLimit HW_LOCK_ILIMIT_A = CurrentLimit::LIMIT_5_0_A;   // hardware lock (just above that)

// The EEPROM (limp-home) configuration. i2c_address goes into I2C_TARGET_ADDR, so pass the address in use.
Config board_config(const MotorProfile &motor, uint8_t i2c_address);

// board_config with direction taken over by the ESP (DIR_INPUT override). ccw = counter-clockwise, seen as the chip
// defines it; which one is exhaust depends on the phase wiring.
Config esp_direction_config(const Config &board, bool ccw);

// MAX_SPEED field for this profile (= electrical Hz x 6).
unsigned max_speed_field(const MotorProfile &motor);

inline float electrical_hz_to_rpm(float hz, const MotorProfile &motor) {
  return hz * 60.f / float(motor.motor_poles / 2);
}
inline float rpm_to_speed_fraction(float rpm) { return rpm / MAX_SPEED_RPM; }

}  // namespace mcf8316
