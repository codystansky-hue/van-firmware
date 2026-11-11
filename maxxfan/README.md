# MaxxFan controller (ESP32-C3)

Lid motor and fan control for the MaxxFan BLDC retrofit. The mechanical / CAD side (boxes, harnesses, the aqx BLDC
driver, the 74AHCT125 buffer) lives in the aiCAD project, `S:\aiCAD\maxxfan_bldc` (`HARNESS_BOM.md`, `WIRING.md`,
`DESIGN.md`).

Board: **ESP32-C3 SuperMini (HW-466AB)**, 22.5 x 18 mm, USB-C, headers soldered.
Firmware: `maxxfan.ino` **v4.1**: v3.1 plus the **FOC board** (`S:\aiCAD\maxxfan_bldc\pcb\foc_board`) as a fourth fan
wiring, section 9. Wi-Fi and updates are in `../libraries/vannet` (shared with the hub), the motor chip driver in
`../libraries/mcf8316`. Arduino core "esp32" by Espressif 3.3.12; board settings in `sketch.yaml`. 1.17 MB = 59 % of the
1.9 MB app slot.

What it does:
* **Lid** (DRV8871): open / close / stop / nudge from the phone page, the BOOT button, an optional panel button, or USB
  serial. Timed moves (no position sensor), calibrated on the page.
* **Fan**: speed, exhaust / intake, from the phone **and** the knob + switch; whichever you touched last wins. Only runs
  with the lid open. Either through the aqx BLDC board (buffer) or, on the FOC board, the MCF8316D motor chip over I2C.
* **Wi-Fi**: joins the van router (Starlink), so the phone stays on it: **http://maxxfan.local**. Its own **MaxxFan**
  hotspot is a fallback when the router is off.
* **Updates over Wi-Fi** from the phone or a PC, with automatic rollback.

## 1. Pins

| ESP pin | to | used in fan mode |
|---|---|---|
| **G6** | DRV8871 IN1 (H5, white) | always |
| **G5** | DRV8871 IN2 (H5, grey) | always |
| **G4** | buffer pin 2 (1A) -> aqx **VR** (PWM speed) | Phone, Phone + knob |
| **G1** | buffer pin 5 (2A) -> aqx **EL** (HIGH = run) | Phone, Phone + knob |
| **G3** | buffer pin 9 (3A) -> aqx **ZF** (HIGH = forward = exhaust) | Phone, Phone + knob |
| **G10** | M divider middle (aqx M -> 10k -> G10 -> 20k -> GND), speed pulses. **Was G0 up to v2: move that wire.** | Phone, Phone + knob |
| **G0** | panel port knob divider (section 3) | Phone + knob |
| **G7** | panel port direction divider (section 3) | Phone + knob |
| **G20** | optional panel lid button, other side to GND | always (unwired = ignored) |
| GND | DRV8871 logic GND, buffer pin 1 | |
| USB-C | 12 -> 5 V converter's USB-C lead | power |
| G8 / G9 | on-board LED / BOOT button | don't connect |

Do not connect anything to G2, G8, G9 (boot pins), or to the 5V pin while USB is in. In fan mode **Knob only** the
ESP leaves every fan pin (G0, G1, G3, G4, G7, G10) as an input.

## 2. Fan wiring stages

Pick the stage on the setup page (**http://maxxfan.local/setup**, section 6); the ESP restarts into it.

| stage | hardware | setup page | who runs the fan |
|---|---|---|---|
| **now** | knob harness H11 in the aqx plug (H1); no buffer | **Knob only** (default) | the knob, straight to the fan board; the ESP only runs the lid |
| buffer built | buffer + ESP ribbon in the aqx plug; H11 unplugged | **Phone** | the phone |
| + panel port | as above, plus H11 in the new **panel port** (section 3) | **Phone + knob** | knob **and** phone, whichever was touched last |
| ESP dead (limp home) | ribbon out of the aqx plug, H11 into it | (any) | the knob, no ESP; the lid only by hand |
| **FOC board** | the FOC board replaces the aqx, buffer, dividers and lid module; H11 in its panel socket J3 | **FOC board** (picked by itself, section 9) | knob **and** phone, as Phone + knob |

In **Phone + knob** (and on the FOC board):
* **The knob's travel** (v4.1), from the bottom:

  | knob | lid | fan |
  |---|---|---|
  | **at its click** for 3 s | closes | off |
  | **vent**: just past the click, about the first 15 % of the travel | opens | off |
  | **beyond vent**, to full | opens first | speed 1 to 100 % (FOC board: 200 rpm to the top speed) |

  Only a knob movement acts: a knob left at its click doesn't close a lid the phone opened. Turning it back up within the
  3 s cancels the close; so does any phone or button command.
* Turning the knob (5 % or more), moving it between those parts, or flipping the direction switch hands the fan to the
  panel; it then follows the knob.
* A phone (or hub) command hands it to the phone until the knob is turned again. The status line shows
  `set by phone, knob at 40%`.
* Closing the lid cancels a phone request. With the knob up, the fan starts again when the lid reopens.
* At power-up the knob position runs the fan only if the lid is already open; the ESP never moves the lid by itself at boot.
* The fan only runs with the lid open and still. A new request opens the lid first. Reversing turns the drive off for 3 s
  before the direction flips.

## 3. The panel port (for Phone + knob)

A second 1x6 **female** DuPont housing, like the aqx plug, on a short pigtail in the BL box. The existing H11 knob
harness plugs into it unchanged, so it can always go back into the aqx plug (limp home).

| pos | H11 brings | panel port wiring |
|---|---|---|
| 1 GND | pot black + switch black | cGND (buffer GND) |
| 2 VR | knob wiper, 0-5 V | **12k** to node K; **10k** K -> GND; K -> ribbon -> **G0** (optional 100 nF K -> GND) |
| 3 ZF | switch common: 5 V = forward, 0 V = reverse | **10k** to node D; **20k** D -> GND; D -> ribbon -> **G7** |
| 4, 5 | -- | empty |
| 6 5V | the pot and switch supply | **aqx 5V** (the pin that feeds the buffer VCC) |

* The ribbon goes from 5 to **7 wires**: GND, G4, G1, G3, **G10** (M), **G0** (knob), **G7** (direction).
* Knob range: the divider gives about 2270 mV at full. The setup page shows the live reading (`Knob now: 47 % (1180 mV)`).
  If full knob doesn't reach 100 %, or reaches it early, change `KNOB_FULL_MV` in the sketch (now 2150). The vent band
  ends at `KNOB_VENT_MV` (420); the click is under `KNOB_OFF_MV` (120). The page's Settings > Fan shows the reading.
* Optional panel lid button: a momentary push button from **G20** to GND, with 100 nF across it on a long run. Tap = open
  if closed, close if open, stop if moving (same as BOOT).
* **CAD / harness docs not updated yet**: `HARNESS_BOM.md`, `WIRING.md` and the buffer build card in aiCAD still show
  the v12.1 5-wire ribbon with M on G0 and no panel port.

## 4. Flash over USB (first time, and the v1 -> v3 step)

v3 uses a different flash layout from v1 (two firmware slots for updates over Wi-Fi), so the step from v1 needs USB
once. The lid calibration survives (same settings area). After that, use section 7.

### Let Claude do it
Plug the ESP into the PC with a USB-C **data** cable and say "flash the MaxxFan ESP".

### arduino-cli (board settings come from `sketch.yaml`)
```
arduino-cli compile --libraries libraries maxxfan
arduino-cli upload  -p COM5 maxxfan
arduino-cli monitor -p COM5 -c baudrate=115200
```
On this PC arduino-cli is `%USERPROFILE%\tools\arduino-cli\arduino-cli.exe`.

### Arduino IDE 2
Install **esp32 by Espressif Systems** 3.x (boards manager URL
`https://espressif.github.io/arduino-esp32/package_esp32_index.json`). Open `maxxfan\maxxfan.ino`. **Tools**: ESP32C3 Dev
Module, USB CDC On Boot **Enabled**, Flash Size 4MB, Partition Scheme **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**.
Copy the repo's `libraries\vannet` folder into `Documents\Arduino\libraries` once (the IDE doesn't take
`--libraries`). Upload, then Serial Monitor at 115200: `MaxxFan ESP v3.1 ... ready`.

### If the upload fails ("No serial data received", "Failed to connect")
**Hold BOOT, tap RESET (or plug in USB while holding BOOT), release BOOT**, upload again, tap RESET after it finishes.

### From a laptop with nothing installed (browser)
1. GitHub > **Actions** > the latest **build** run > download **maxxfan-usb-full-image** and unzip it:
   `maxxfan.ino.merged.bin` (4 MB).
2. In Chrome or Edge open **https://espressif.github.io/esptool-js/**, **Connect**, pick the SuperMini's port ("USB
   JTAG/serial debug unit"; if nothing shows, use the BOOT / RESET trick above).
3. Flash Address **0x0**, choose `maxxfan.ino.merged.bin`, **Program**. When it's done, tap RESET or replug.

This writes the whole flash, so the ESP **starts from defaults**: the MaxxFan hotspot again (section 5), and the lid
needs calibrating again. `arduino-cli upload` (above) and updates over Wi-Fi (section 7) keep all that.

## 5. First use

### Wi-Fi (once)
1. Power the ESP (USB-C) and the DRV8871 (12 V). The blue LED blinks every 2 s when idle, stays on while the lid moves.
2. With no router saved the ESP runs its hotspot. On the iPhone join **MaxxFan** (password `maxxfan123` until you change
   it) and open **http://192.168.4.1**.
3. **Wi-Fi** box: **scan**, pick the Starlink network (or type it), its password, **save + join**. The line changes to
   `on <network> (192.168.1.x, -50 dBm)`.
4. Back on Starlink, open **http://maxxfan.local**, then **Share > Add to Home Screen**. The hotspot turns off a minute
   after the phone leaves it.
5. iPhone **Settings > Wi-Fi > (i) next to MaxxFan > Auto-Join off**, so the phone doesn't sit on the hotspot after
   Starlink comes back.
6. **http://maxxfan.local/setup** (user `maxxfan`, password `maxxfan-ota`): set your own **passwords**. They are kept on
   the ESP, not in this repo, and survive updates.

### Lid
Calibration is under **Settings** (bottom of the fan page) > **Lid**.
1. **Nudge open** (0.5 s). If it moved the wrong way: tick **Swap the lid's direction**, **Save lid settings**.
2. **Calibrate**: nudge fully closed, **Set closed**; go back, press **Open** and time it to fully open (**Stop** if it
   stalls); enter that as **full travel, ms**, keep **extra at the ends** ~600, **Save lid settings**.
3. The lid button on the main screen (**Open** / **Close**) then runs the calibrated time; **Stop** stops instantly; the
   position estimate survives power-off.
4. USB serial (115200): `o` open, `c` close, `s` stop, `j` / `J` nudge, `?` status.

If the lid motor stalls hard at the ends (buzzing, hot), shorten **extra ms**.

## 6. Wi-Fi and the setup page

| situation | the ESP | the fan page |
|---|---|---|
| no router saved | MaxxFan hotspot only | http://192.168.4.1 on MaxxFan |
| router up | on the router, hotspot off | **http://maxxfan.local** (or the router IP on the page) |
| router off / unreachable for 20 s | hotspot on; retries the router every minute (not while a phone is on the hotspot) | http://192.168.4.1 |
| router back | rejoins; hotspot off once nobody has been on it for a minute | http://maxxfan.local |

* Starlink keeps its local Wi-Fi up without a satellite link. The ESP is 2.4 GHz only: Starlink's default network is fine;
  if you split the bands, save the 2.4 GHz one. Wi-Fi power is 8.5 dBm (SuperMini boards often fail at full power).
* **Setup page** (`/setup`, basic login `maxxfan` + the setup password): firmware upload, fan wiring stage (section 2), passwords.

## 7. Firmware updates over Wi-Fi

The file is **`maxxfan.ino.bin`** (~1.1 MB), never `.merged.bin` / `.bootloader.bin` (the ESP rejects those anyway).

### From the iPhone
1. Get the `.bin`: GitHub **Actions** > latest **build** run > **maxxfan-firmware** (a zip; tap it in Files to unzip),
   or the **Releases** page for a tagged version (`maxxfan-v3.bin`), or Claude sends it to you.
2. **http://maxxfan.local/setup** > Firmware update > pick the file > **Upload and restart** (~20 s).

### From a PC on the same network
```
arduino-cli compile --libraries libraries --output-dir build/maxxfan maxxfan
"%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.12\tools\espota.exe" -i maxxfan.local -a <setup password> -f build\maxxfan\maxxfan.ino.bin -r
curl http://maxxfan.local/api/status
```
The last line confirms the new firmware (below). Windows Firewall asks about `espota.exe` the first time: allow it on
private networks (the ESP connects back to the PC). If `maxxfan.local` doesn't resolve, use the IP on the fan page.

### What protects you
* The lid and fan stop before anything is written. During the restart the DRV8871 inputs are pulled low (lid coasts) and
  the buffer's pull-downs stop the fan.
* A corrupt, interrupted or wrong file is rejected; the old firmware keeps running.
* New firmware runs **on trial** until it answers a web request (the fan page, or `GET /api/status`); the update page
  sends you there. No answer within 10 min, a crash or a power cut before then -> the ESP goes back to the old firmware.
* Keep the USB-C reachable: a firmware that loads the page but breaks updates (or the lid) passes the trial.

## 8. HTTP API (for the phone page and the hub)

| request | does |
|---|---|
| `GET /api/status` | JSON: `state`, `pos` (lid %, -1 unknown), `fan_mode` (0 knob only, 1 phone, 2 phone + knob, 3 FOC board), `fan` (requested %), `fan_out` (running %), `dir` (`fwd` = exhaust / `rev`), `fan_src` (`knob` / `phone`), `fan_wait` (`lid` / `reverse` / ""), `m_hz` (FOC board: motor rev/s from FG), `knob` (fan % the knob asks for), `knob_mv`, `knob_zone` (`off` / `vent` / `fan`, "" without a knob), `wifi_*`, `ap`, `fw`. FOC board also `foc` {`up`, `state`, `rpm`, `want_rpm`, `sent_rpm`, `vm`, `bus_a`, `phase_a`, `fault`, `eeprom` (`board` / `other` / `unknown`)}, `top_rpm`, `fan_swap`, `steps` (the slider's 10 speeds, rpm) |
| `GET /api/lid?cmd=open\|close\|stop\|jog_open\|jog_close\|set_closed[&ms=500]` | lid |
| `GET /api/fan?speed=0..100&dir=fwd\|rev` | fan, as a phone request (opens the lid if needed). FOC board: `rpm=200..1500` instead of `speed` |
| `GET /api/settings?travel=&over=&ramp=&invert=0\|1` | lid calibration; FOC board also `top_rpm=400..1500`, `fan_swap=0\|1` |
| `GET /api/history` | FOC board: the last 10 min of motor readings, one every 2 s, oldest first: `dt` (s), `age` (s since the newest), `rpm`, `set` (rpm asked for), `vm` (V x 10), `bus` (mA); `null` where the chip wasn't answering. Kept in RAM only |
| `GET /api/foc?cmd=clear` | FOC board: clear the motor chip's faults |
| `POST /api/foc_save` | FOC board: write the knob fallback into the motor chip's EEPROM (basic auth; fan stopped) |
| `POST /api/wifi` `ssid`, `pass` / `forget=1` | van router |
| `GET /api/scan` | `{"busy":true}` until `{"nets":[...]}` |
| `GET /setup`, `POST /api/setup` `fanmode`, `ap_pass`, `ota_pass` | setup (basic auth), restarts |
| `POST /update` multipart `.bin` | firmware (basic auth) |
| UDP/TCP 3232 | ArduinoOTA / espota, password = setup password |

## 9. The FOC board (fan mode 3)

The FOC board (`S:\aiCAD\maxxfan_bldc\pcb\foc_board`, rev A) replaces the aqx, the buffer, the dividers and the lid
module. Its TI **MCF8316D** runs the fan motor sensorless (field-oriented control, much quieter than the aqx), with
Minuet's profile for this exact motor in a MaxxFan Deluxe (`../libraries/mcf8316/src/FocMotor.cpp`). The ESP plugs into
the board's sockets and sets the speed and direction over I2C. These go into the chip's RAM only, so a power cycle always
starts from the chip's own EEPROM.

| ESP pin | FOC board |
|---|---|
| G0 / G1 | MCF8316D I2C SCL / SDA (address 0x01) |
| G3 | FG: one pulse per motor revolution (`m_hz`) |
| G4 | knob divider, which is also the chip's SPEED pin (input only) |
| G7 | direction-switch divider, also the chip's DIR pin |
| G10 | nFAULT (the board's red LED) |
| G5 / G6, G20, G21 | lid DRV8871 IN2 / IN1, panel lid button, DS18B20 (not read yet) |

**Picked by itself**: at start-up v4 looks for the chip on G0 / G1. If it answers, the fan mode becomes **FOC board**
and stays that way. (The chip runs from the same 12 V as the ESP; with the ESP on USB and the 12 V off, it isn't found.)

### First run, with the motor and lid installed

1. **Get v4 on the ESP**, keeping its settings: http://maxxfan.local/setup > Firmware update > `maxxfan.ino.bin` (section
   7), if it runs v3.x. Otherwise use USB (section 4).
2. **Fan page**: the **Motor** card graphs speed, current (with watts) and supply voltage over the last 2 or 10 minutes;
   drag a finger along it to read any moment, or open **Readings** for the numbers. The bottom line shows
   `motor stopped · 12.x V`; **Settings > Motor** has the details (state, rpm, supply,
   faults, whether the knob fallback is saved). A red box saying `Motor chip not answering` means the 12 V is off or the
   chip didn't start; the serial console (`?`) says more.
3. **Lid**: check its direction and calibrate it (section 5) if that isn't done yet. The fan only runs with the lid fully
   open.
4. **Speed 3 (400 rpm), Exhaust** on the main screen, or **Settings > Fan > run at 400, Run exhaust**. The lid opens first
   if needed. You hear a small tick (the chip finding the rotor), then it spins up. Settings > Motor shows the state go
   `IPD`, `OPEN_LOOP`, `CLOSED_LOOP_UNALIGNED`, `CLOSED_LOOP_ALIGNED`, and the speed settle near 400, with `FG` agreeing.
5. **Direction**: if exhaust blows in, swap two of the three motor wires in J2 (that fixes the knob fallback too). The
   **Swap the fan's direction** box (Settings > Fan) does it in software, for the ESP only.
6. **Speeds**: the slider's 10 speeds are Minuet's (200, 300 ... 900, 1000, 1200 rpm), scaled so speed 10 = the **top
   speed** (Settings > Fan: 1200 rpm, Minuet's top; up to 1500). The knob runs from 200 rpm, the slowest it runs
   sensorless, to the top speed. Listen for hunting or buzzing.
7. **Knob**: the knob and the phone both work, and the knob runs the lid too (section 2): click 3 s = close, vent = lid
   open with the fan off.
8. **Check the real speed** once, with a phone tachometer or strobe app. If it's half or double the page's rpm, the motor's
   pole count (4 assumed) is wrong.
9. **When it all runs well**: setup page > **Save the knob fallback into the motor chip**, once. After that the knob and
   switch run the fan with the ESP dead or unplugged (limp home; the lid only by hand), to about 1500 rpm at the top.
   With no ESP there's no vent band: the bottom of the knob just runs the fan slowly.
   This writes the chip's EEPROM (about 20 000 writes in its life), and only if it differs. Until it's saved, a dead
   ESP means no fan.

A fault shows a red box on the main screen saying why (`Motor stopped: it lost track of the rotor`), with the chip's
fault names underneath, and the board's red LED comes on. The chip retries 5 times, 1 s apart, then stops. **Clear the
motor fault**, or any new fan command, retries it.

| fault | likely |
|---|---|
| `NO_MTR` | a motor wire isn't making contact in J2 |
| `IPD_T1_FAULT`, `IPD_T2_FAULT`, `IPD_FREQ_FAULT` | start-up rotor detection failed: a loose wire, or the blade is stuck |
| `MTR_LCK`, `ABN_SPEED`, `ABN_BEMF` | lost track of the rotor (stall, or a jerk at start-up). Note the speed it happened at |
| `LOCK_ILIMIT`, `HW_LOCK_ILIMIT` | over 4.5 / 5 A: something's binding, or a short |
| `MTR_UNDER_VOLTAGE` | VM under 8 V: a weak supply or thin wiring |
| `OTW`, `OTS` | the chip is hot |

The USB bench-test sketch `../foc_test` drives the same chip from a serial console. It has no Wi-Fi, so after flashing it
the ESP needs USB again to get back to maxxfan.
