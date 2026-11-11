// Ported from brown-studios/minuet-firmware minuet/fan_driver.h, Controller::make_config_ (MIT, see LICENSE.minuet.txt).
// Minuet wrote raw numbers; here each one is the typed value with the meaning Minuet's comment gives. Lines marked
// "board:" differ from Minuet because the FOC board is wired differently.
#include "FocMotor.h"

namespace mcf8316 {

const MotorProfile STEPPERONLINE_57BYA54_12_01 = {
    "StepperOnline 57BYA54-12-01",
    4,    // poles
    0,    // lead angle, degrees
    74,   // MOTOR_RES
    98,   // MOTOR_IND
    103,  // MOTOR_BEMF_CONST
    115,  // SPD_LOOP_KP
    293,  // SPD_LOOP_KI
    IPDClockFrequency::FREQ_1000_HZ,
    IPDCurrentThreshold::THRESHOLD_1_5_A,
    CurrentLimit::LIMIT_2_0_A,                 // open loop
    OpenLoopAcceleration::ACCEL_2_5_HZ_S,
    CurrentLimit::LIMIT_4_0_A,                 // closed loop
    ClosedLoopSlowAcceleration::ACCEL_3_0_HZ_S,
    ClosedLoopAcceleration::ACCEL_20_0_HZ_S,
    ClosedLoopDeceleration::ACCEL_NO_LIMIT,
    {200, 300, 400, 500, 600, 700, 800, 900, 1000, 1200},
};

unsigned max_speed_field(const MotorProfile &motor) {
  const float electrical_hz = MAX_SPEED_RPM / 60.f * float(motor.motor_poles / 2);
  return unsigned(electrical_hz * 6.f + 0.5f);  // 2000 rpm, 4 poles -> 400
}

Config board_config(const MotorProfile &m, uint8_t i2c_address) {
  Config c{};  // everything not set below is 0, as in Minuet

  // ---- inputs (board: Minuet drives SPEED/WAKE from the ESP, sleeps, and sets speed and direction over I2C)
  c.set(I2C_TARGET_ADDR, i2c_address);
  c.set(SPEED_MODE, SpeedMode::SPEED_PIN_ANALOG);          // board: the knob divider on SPEED
  c.set(INPUT_REFERENCE_MODE, InputReferenceMode::SPEED);
  c.set(BRAKE_INPUT, BrakeInput::OVERRIDE_OFF);            // BRAKE pin is grounded anyway
  c.set(DIR_INPUT, DirInput::DIR_PIN);                     // board: the direction-switch divider on DIR
  c.set(DEV_MODE, DeviceMode::STANDBY);                    // board: never sleep, so I2C always answers
  c.set(SLEEP_ENTRY_TIME, SleepEntryTime::SLEEP_AFTER_20_MS);
  c.set(EXT_WDT_EN, false);  // TI erratum: the I2C watchdog fights BRAKE (Minuet hit it)

  // ---- motor
  c.set(FG_DIV, fg_div_from_motor_poles(m.motor_poles));
  c.set(LEAD_ANGLE, lead_angle_from_degrees(m.lead_angle_deg));
  c.set(MOTOR_RES, m.motor_res);
  c.set(MOTOR_IND, m.motor_ind);
  c.set(MOTOR_BEMF_CONST, m.motor_bemf_const);
  c.set(SPD_LOOP_KP, m.spd_loop_kp);
  c.set(SPD_LOOP_KI, m.spd_loop_ki);

  // ---- speed input: reference = input as-is, no clamps, windows or hysteresis
  c.set(MAX_SPEED, max_speed_field(m));
  c.set(INPUT_REFERENCE_WINDOW, InputReferenceWindow::WINDOW_DISABLED);
  c.set(REF_PROFILE_CONFIG, ReferenceProfileConfig::EQUATION);
  c.set(DUTY_CLAMP1, 0u);
  c.set(DUTY_HYS, 0u);
  c.set(MIN_DUTY, 0u);
  c.set(REF_CLAMP1, 0u);

  // ---- initial speed detection (catch a blade that's already turning)
  c.set(ISD_EN, true);
  c.set(BRAKE_EN, true);
  c.set(HIZ_EN, false);
  c.set(RESYNC_EN, true);
  c.set(FW_DRV_RESYN_THR, ForwardDriveResyncThreshold::THRESHOLD_20_PERCENT);
  c.set(STAT_DETECT_THR, StationaryDetectionThreshold::THRESHOLD_50_MV);
  c.set(ISD_BEMF_FILT_ENABLE, true);
  c.set(ISD_STOP_TIME, ISDStopTime::TIME_50_MS);
  c.set(ISD_RUN_TIME, ISDRunTime::TIME_50_MS);
  c.set(ISD_TIMEOUT, ISDTimeout::TIME_1000_MS);
  c.set(BRAKE_CURRENT_PERSIST, BrakeCurrentPersistence::TIME_500_MS);
  c.set(FAST_ISD_EN, false);

  // ---- start-up with initial position detection (a small "tick", never starts backwards)
  c.set(MTR_STARTUP, MotorStartup::IPD);
  c.set(IPD_CLK_FREQ, m.ipd_clk_freq);
  c.set(IPD_CURR_THR, m.ipd_curr_thr);
  c.set(IPD_RLS_MODE, IPDReleaseMode::TRISTATE);
  c.set(IPD_ADV_ANGLE, IPDAdvanceAngle::ANGLE_30_DEG);
  c.set(IPD_REPEAT, IPDRepeat::REPEAT_3);
  // Minuet asks for IPD_HIGH_RESOLUTION_EN = 1, but upstream's register map (b8b6d54) puts that field on bit 1, which is
  // CIRCULAR_CURRENT_LIMIT_ENABLE (also 1), so Minuet's config leaves the real bit 0 clear. Keep it clear, as Minuet
  // runs, until it's been tried on. (Checked: every other register is bit-identical to Minuet's.)
  c.set(IPD_HIGH_RESOLUTION_EN, false);
  c.set(IPD_TIMEOUT_FAULT_EN, true);
  c.set(IPD_FREQ_FAULT_EN, true);

  // ---- open loop
  c.set(OL_ILIMIT, m.ol_ilimit);
  c.set(OL_ACC_A1, m.ol_acc_a1);
  c.set(OL_ACC_A2, OpenLoopAcceleration2::ACCEL_0_0_HZ_S2);
  c.set(FIRST_CYCLE_FREQ_SEL, FirstCycleFrequencySelect::FREQ_0_HZ);
  c.set(ALIGN_SLOW_RAMP_RATE, AlignSlowRampRate::LIMIT_50_A_S);

  // ---- open -> closed loop handoff
  c.set(AUTO_HANDOFF_EN, true);
  c.set(AUTO_HANDOFF_MIN_BEMF, AutoHandoffMinBEMF::BEMF_100_MV);
  c.set(OPN_CL_HANDOFF_THR, OpenToCloseLoopHandoffThreshold::THRESHOLD_25_PERCENT);  // MPET only (auto handoff)
  c.set(IQ_RAMP_EN, false);
  c.set(LOW_SPEED_RECIRC_BRAKE_EN, true);
  c.set(THETA_ERROR_RAMP_RATE, ThetaErrorRampRate::RATE_0_10_DEG_MS);

  // ---- closed loop
  c.set(ILIMIT, m.ilimit);
  c.set(OVERMODULATION_ENABLE, false);  // quieter
  c.set(CL_SLOW_ACC, m.cl_slow_acc);
  c.set(CL_ACC, m.cl_acc);
  c.set(CL_DEC, m.cl_dec);
  c.set(MTR_STOP, MotorStop::RECIRCULATE);  // keeps the blade's energy off the 12 V bus
  c.set(MTR_STOP_BRK_TIME, MotorStopBrakeTime::TIME_5000_MS);

  // ---- braking (slow: the blade has a lot of inertia)
  c.set(BRK_MODE, BrakeMode::BRAKE_LOW_SIDE);
  c.set(BRK_CONFIG, BrakeConfig::BRAKE_CURRENT_AND_TIME);
  c.set(BRK_CURR_THR, BrakeCurrentThreshold::BRAKE_CURRENT_0_1_A);
  c.set(BRK_TIME, BrakeTime::BRAKE_TIME_10000_MS);
  c.set(BRAKE_SPEED_THRESHOLD, BrakeSpeedThreshold::THR_20_PERCENT);
  c.set(BRAKE_PIN_MODE, BrakePinMode::LOW_SIDE_BRAKE);
  c.set(ACTIVE_BRAKE_EN, false);

  // ---- direction change: brake, then start the other way (no reverse drive, no flux weakening)
  c.set(DIR_CHANGE_MODE, DirChangeMode::STOP);
  c.set(RVS_DR_EN, false);
  c.set(FLUX_WEAK_ENABLE, false);

  // ---- faults: retry 5 times, 1 s apart, then latch
  c.set(LCK_RETRY, LockRetryTime::TIME_1000_MS);
  c.set(AUTO_RETRY_TIMES, AutoRetryTimes::TIMES_5);
  c.set(LOCK_ILIMIT, LOCK_ILIMIT_A);
  c.set(LOCK_ILIMIT_DEG, LockCurrentLimitDeglitch::TIME_0_2_MS);
  c.set(LOCK_ILIMIT_MODE, LockMode::AUTO_RETRY_HI_Z);
  c.set(HW_LOCK_ILIMIT, HW_LOCK_ILIMIT_A);
  c.set(HW_LOCK_ILIMIT_DEG, HWLockCurrentLimitDeglitch::TIME_2_US);
  c.set(HW_LOCK_ILIMIT_MODE, LockMode::AUTO_RETRY_HI_Z);
  c.set(LOCK1_EN, true);  // abnormal speed
  c.set(LOCK2_EN, true);  // abnormal BEMF
  c.set(LOCK3_EN, true);  // no motor
  c.set(LOCK_ABN_SPEED, AbnormalSpeedThreshold::THR_130_PERCENT);
  c.set(ABNORMAL_BEMF_THR, AbnormalBEMFThreshold::THR_65_PERCENT);
  c.set(ABNORMAL_BEMF_PERSISTENT_TIME, AbnormalBEMFPersistentTime::TIME_500_MS);
  c.set(NO_MTR_THR, NoMotorCurrentThreshold::THR_0_0375_A);
  c.set(NO_MTR_FLT_CLOSEDLOOP_DIS, false);
  c.set(MTR_LCK_MODE, LockMode::AUTO_RETRY_HI_Z);

  // ---- gate driver and modulation (quiet: 25 kHz, random dither, spread spectrum)
  c.set(CIRCULAR_CURRENT_LIMIT_ENABLE, true);
  c.set(AVS_EN, true);
  c.set(DEADTIME_COMP_EN, true);
  c.set(SLEW_RATE, SlewRate::SLEW_200_V_US);
  c.set(MIN_ON_TIME, MinOnTime::TIME_0_50_US);
  c.set(DYNAMIC_CSA_GAIN_EN, true);
  c.set(DYNAMIC_VOLTAGE_GAIN_EN, false);
  c.set(SPREAD_SPECTRUM_MODULATION_DIS, false);
  c.set(PWM_MODE, PWMMode::CONTINUOUS);
  c.set(PWM_FREQ_OUT, PWMOutputFrequency::FREQ_25_KHZ);
  c.set(PWM_DITHER_MODE, PWMDitherMode::RANDOM);
  c.set(PWM_DITHER_DEPTH, PWMDitherDepth::DEPTH_5_PERCENT);

  // ---- internal buck off: FB_BK takes the board's 5 V (U3). BUCK_SEL 3.3 V keeps the FB_BK undervoltage trip at
  // ~2.6 V, well under the 5 V rail. BUCK_PS_DIS: AVDD from VM, DVDD from FB_BK.
  c.set(BUCK_DIS, true);
  c.set(BUCK_PS_DIS, true);
  c.set(BUCK_SEL, BuckVoltage::OUTPUT_3_3_V);
  c.set(BUCK_CL, BuckCurrentLimit::LIMIT_150_MA);

  // ---- 12 V bus
  c.set(BUS_VOLT, BusVoltage::BUS_30_V);
  c.set(VOLTAGE_HYSTERESIS, VoltageHysteresis::HYSTERESIS_1_0_V);
  c.set(MIN_VM_MOTOR, MinVMMotor::THR_8_V);
  c.set(MIN_VM_MODE, MinMaxVMMode::AUTO_CLEAR);
  c.set(MAX_VM_MOTOR, MaxVMMotor::THR_18_V);
  c.set(MAX_VM_MODE, MinMaxVMMode::AUTO_CLEAR);
  c.set(VDC_FILTER, VDCFilter::DEFAULT);
  c.set(MAX_POWER, max_power_from_watts(BUS_POWER_LIMIT_W));
  c.set(BUS_POWER_LIMIT_ENABLE, true);
  c.set(OVP_SEL, OvervoltageLevel::LIMIT_22_V);
  c.set(OVP_EN, true);
  c.set(OTW_REP, true);
  c.set(OCP_DEG, OvercurrentDeglitch::TIME_0_6_US);
  c.set(OCP_LVL, OvercurrentLevel::LIMIT_16_A);
  c.set(OCP_MODE, OvercurrentMode::LATCHED_FAULT);

  // ---- pins (board: FG -> G3 and nFAULT -> G10 have 10k pull-ups to 3V3, so the internal ones stay off)
  c.set(ALARM_PIN_EN, false);
  c.set(FG_SEL, FGSelect::FG_IN_ISD_OPEN_AND_CLOSED_LOOP);
  c.set(FG_CONFIG, FGConfig::FG_WHILE_DRIVEN);
  c.set(PULLUP_ENABLE, false);
  c.set(SPEED_PIN_GLITCH_FILTER, SpeedPinGlitchFilter::TIME_0_2_US);
  c.set(SLEW_RATE_I2C_PINS, SlewRateI2CPins::SLEW_4_8_MA);

  // ---- EEPROM and misc
  c.set(EEP_FAULT_MODE, EEPROMFaultMode::LATCHED_FAULT);
  c.set(EEPROM_LOCK_MODE, EEPROMLockMode::READ_OPEN_AND_WRITE_OPEN);
  c.set(CRC_ERR_MODE, CRCErrorMode::LATCHED_FAULT_HI_Z);  // a garbled I2C write stops the motor
  c.set(SATURATION_FLAGS_EN, false);
  c.set(EXT_CLK_EN, false);
  return c;
}

Config esp_direction_config(const Config &board, bool ccw) {
  Config c = board;
  c.set(DIR_INPUT, ccw ? DirInput::OVERRIDE_COUNTER_CLOCKWISE : DirInput::OVERRIDE_CLOCKWISE);
  return c;
}

}  // namespace mcf8316
