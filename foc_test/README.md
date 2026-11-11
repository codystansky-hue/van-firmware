# FOC board bench test

**For the fan as installed, use maxxfan v4 instead** (`../maxxfan`, section 9): phone page, lid, updates over Wi-Fi.
This sketch has no Wi-Fi, so once it is flashed the ESP needs USB again to get back to maxxfan.

Spins the fan motor (StepperOnline 57BYA54-12-01) on the FOC board's MCF8316D from a serial console on the
SuperMini's USB-C. It isn't the fan firmware: there's no Wi-Fi, no lid, no knob logic. It's for the first spins, before
v4.

Board: `S:\aiCAD\maxxfan_bldc\pcb\foc_board` (rev A). Driver and motor profile: [`libraries/mcf8316`](../libraries/mcf8316/src/FocMotor.h).
The profile is Minuet's tuning for this exact motor in a MaxxFan Deluxe ([brown-studios/minuet-firmware](https://github.com/brown-studios/minuet-firmware),
MIT). Every chip register is bit-identical to Minuet's except four: speed from the knob, direction from the switch,
standby instead of sleep, and IPD high-resolution (left off, the way Minuet's chips actually run).

## What it does to the chip

When it finds the chip (and again whenever the chip comes back):

1. It takes the speed command away from the SPEED pin (the knob), at speed 0.
2. It reads the chip's EEPROM and says whether it already holds the board profile. A new chip won't.
3. It loads the profile into the chip's **RAM**, with direction from the ESP.

**Nothing goes into the EEPROM** unless you type `save yes`. Switching off the 12 V wipes everything else.

## Before

* The board has passed the meter checks in the board README ("First power-up": SuperMini out, motor unplugged).
* **Motor clamped on the bench, blade off** for the first spins: it kicks when it starts. Phases into J2, any order.
  Hall wires insulated and tucked away.
* **Panel (J3) and lid motor (J5) unplugged.** With J3 out, R6 holds the SPEED pin at 0 V, so a new chip can't start
  the motor on its own, whatever its factory setup is.
* 12 V from a bench supply limited to about 3 A, or from the battery through the 5 A fuse.
* SuperMini in its sockets, chip side up, USB-C at the board's right edge.

## Flash and open the console

Plug the SuperMini into the PC. It shows up as "USB JTAG/serial debug unit" on some COM port; COM4 on Cody's PC is the
CH340 adapter, not this. Then:

```
arduino-cli compile --libraries libraries --output-dir build/foc_test foc_test
arduino-cli upload -p COMx foc_test
arduino-cli monitor -p COMx -c baudrate=115200
```

If the upload can't connect, hold BOOT, tap RESET, let go of BOOT, then upload again. (Or plug it in and ask Claude to
flash it and run the steps below.)

## The test

Type a command and press Enter. An empty line stops the motor.

1. **Turn the 12 V on.** Expect `MCF8316D answered at 0x01`, a line about what the EEPROM holds, `Loaded ... profile
   into RAM`, and `Ready`. The green LED (D4) is on and the red one (D5) is off.
2. **`rpm 300`.** You hear a tick, then it spins up. The state goes `IPD`, `OPEN_LOOP`, `CLOSED_LOOP_UNALIGNED`, then
   `CLOSED_LOOP_ALIGNED`. Each status line shows:
   * `chip`: the chip's own speed estimate. It should settle near 300 rpm.
   * `FG`: the speed pulses counted on G3. They should agree with `chip`.
   * `bus`: well under an amp with no blade.
   * `VM`: your supply voltage.
3. **`rpm 600`, `rpm 1000`, `rpm 1200`**, then **`stop`**. It slows to a stop by itself, without braking.
4. **`dir ccw`**, then `rpm 300`. It runs the other way. `dir cw` puts it back.
5. **Check the real speed** with a phone tachometer or strobe app, or a tape flag and slow-motion video. If the real
   speed is half or double the `chip` figure, the pole count in the profile is wrong (4 is assumed). Note it.
6. **With the blade, in the fan**: run 200 up to 1200 rpm. Note which direction blows out (exhaust), and listen.
   Minuet's fan speeds are `speed 1` to `speed 10` (200, 300, ... 1000, 1200 rpm).
7. **Optional, limp-home rehearsal (RAM only)**: plug the panel into J3, knob down, type `knob`. Now the knob and switch
   drive the motor the way they will with the ESP dead, to about 1500 rpm at the top. `esp` takes it back.
8. **When it all runs well**: `save yes`. This writes the board profile into the EEPROM once. After that the knob runs
   the fan even with no ESP. It only writes if the EEPROM differs. The EEPROM is good for about 20 000 writes, so
   don't make a habit of it.

**To stop in a hurry, switch off the 12 V.** If the console or I2C link dies while the motor runs, the chip keeps the
last speed it was given. The test prints `Lost the MCF8316D` when that happens.

## Other commands

| | |
|---|---|
| `s` | status line now. `watch off` stops the once-a-second lines |
| `faults`, `clear` | show / clear the chip's faults (the red LED) |
| `dump`, `dump eeprom` | every config field in the chip's RAM / in the EEPROM snapshot |
| `diff` | RAM against the board profile |
| `mpet` | TI's motor measurement (resistance, inductance, Ke, inertia). Spins the motor; reports next to the profile's numbers; changes nothing |
| `rd 0xE4` | raw register read |
| `find` | look for the chip at every I2C address, if 0x01 doesn't answer |
| `i2c 50` | I2C clock in kHz (100 by default) |

## If it doesn't run

| you see | likely |
|---|---|
| no `answered` line | 12 V off (the chip runs from it), or the SuperMini's not seated. Try `find`. Check SDA and SCL (G1, G0) reach U1 |
| `IPD_T1_FAULT` / `IPD_T2_FAULT` / `IPD_FREQ_FAULT` | start-up position detection failed: a phase wire is loose, or the motor's stalled |
| `NO_MTR` | a phase wire isn't making contact in J2 |
| `MTR_LCK`, `ABN_SPEED`, `ABN_BEMF` | it lost track of the rotor (stall, or a jerk at start-up). Save the log: the acceleration settings may need easing for the blade |
| `LOCK_ILIMIT` / `HW_LOCK_ILIMIT` | over the 4.5 / 5 A current limits: something's binding, or a short |
| `MTR_UNDER_VOLTAGE` | VM dropped below 8 V: the supply's current limit is too low, or the wiring's too thin |
| `OTW` / `OTS` | the chip is hot (warning at 145 C) |
| it starts and then hunts, or buzzes at speed | the speed loop doesn't suit this blade. Run `mpet` with the blade on and save the output |

Faults retry 5 times, 1 s apart, then latch. `clear` resets them.
