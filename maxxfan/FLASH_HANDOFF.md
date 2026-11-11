# Handoff: flash maxxfan v4 onto the FOC board

For Claude Code on the laptop next to the fan. Written 2026-11-10 by the session that wrote v4 (on Cody's desktop, which
isn't near the board).

## Where things are

* **Firmware**: `maxxfan/maxxfan.ino` **v4**, on branch **`maxxfan-v4-foc`** (PR #1, not merged; commit `3298c3b` or
  later). It's v3.1 plus fan mode 3, the FOC board: the MCF8316D motor chip runs the fan motor and the ESP sets speed and
  direction over I2C. Details: `maxxfan/README.md` section 9. Driver and motor profile: `libraries/mcf8316`.
* **Built and checked on the desktop and in CI. Never run on hardware.** This is its first flash.
* **Hardware, as it is now**: the FOC board is installed in the MaxxFan. The fan motor is on J2, **the lid motor is
  plugged in (J5)**, the panel probably on J3, and the ESP32-C3 SuperMini sits in the board's sockets (USB-C at the board
  edge). The board runs from the van's 12 V; the SuperMini also takes USB power, and the board is built for both at once.
* **What's on the ESP now: unknown.** Maybe v3.x, maybe something older. It doesn't matter for a USB upload.

## Rules

1. **Flash `maxxfan` only.** Not `foc_test`: it has no Wi-Fi updates, so the ESP would need USB again.
2. **Use `arduino-cli upload`.** It keeps the ESP's saved Wi-Fi, passwords and lid calibration. Don't erase the flash and
   don't write `maxxfan.ino.merged.bin` (that wipes them) unless arduino-cli can't work at all, and then ask Cody first.
3. **Nothing that moves the fan or the lid unless Cody says go, right then.** They're installed: `/api/fan`, `/api/lid`,
   and the serial keys `o` `c` `j` `J` all move real hardware. `?` only reads.
4. **Never write the motor chip's EEPROM.** No `POST /api/foc_save`. Cody presses that button himself once he's happy.
5. Don't change the Wi-Fi or the passwords. Don't merge the PR or push to `main`.

## Steps

### 1. Tools
* `git`, **arduino-cli** (any 1.x), and the ESP32 core **3.3.12**:
  ```
  arduino-cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  ```
  arduino-cli itself: Windows `winget install ArduinoSA.CLI`, macOS `brew install arduino-cli`, or the release zip from
  https://github.com/arduino/arduino-cli/releases.
* For the serial check in step 5: Python 3 with `pyserial` (`pip install pyserial`).

### 2. Code
```
git clone https://github.com/codystansky-hue/van-firmware   # or git fetch in an existing clone
cd van-firmware
git checkout maxxfan-v4-foc
git log -1 --oneline
```

### 3. Build
```
arduino-cli compile --libraries libraries --output-dir build/maxxfan maxxfan
```
Board settings come from `maxxfan/sketch.yaml` (ESP32C3 Dev Module, USB CDC on boot, Minimal SPIFFS with OTA). Expect
`Sketch uses 1170xxx bytes (59%)`. Any error: stop and report it; don't patch the firmware.

### 4. Upload
1. Ask Cody to plug the SuperMini into the laptop with a USB-C **data** cable, and to have the **12 V on**. The ESP looks
   for the motor chip when it starts, and the chip only runs on 12 V.
2. `arduino-cli board list`. The SuperMini is the ESP32-C3's own USB: VID **303A**, PID **1001**, "USB JTAG/serial debug
   unit" (Windows `COMx`, macOS `/dev/cu.usbmodem…`, Linux `/dev/ttyACM…`). Not a CH340 / CP210x adapter.
3. `arduino-cli upload -p <port> maxxfan`
4. "Failed to connect" / "No serial data received": ask Cody to **hold BOOT, tap RESET, let go of BOOT** on the SuperMini,
   then upload again. After an upload from that mode, ask him to tap RESET.

### 5. Check it started right
The ESP restarts after the upload; give it 5 s. Then send `?` over serial (it only prints status). Open the port with
DTR and RTS low: on this chip they're wired to reset and BOOT.
```python
import serial, sys, time
p = serial.Serial(); p.port = sys.argv[1]; p.baudrate = 115200; p.timeout = 0.2
p.dtr = False; p.rts = False; p.open()
time.sleep(0.3); p.reset_input_buffer(); p.write(b'?')
end = time.time() + 3
while time.time() < end:
    line = p.readline()
    if line: print(line.decode(errors='replace').rstrip())
p.close()
```
Good looks like this (two lines):
```
lid pos 0%, travel 12000 ms, invert 0 | fan mode 3, 0% fwd by knob, knob 0% (3 mV) | fw v4 Nov 10 2026 ... | router "..." up 192.168.x.x | hotspot off
FOC: chip up, IDLE, 0 rpm (sent 0), VM 12.6 V, bus 0.00 A, phase 0.00 A, faults none, EEPROM not the board profile
```
* `fw v4`: the new firmware is running.
* `fan mode 3`: it found the FOC board. If it shows 0, 1 or 2, the 12 V was off when the ESP started: ask Cody to
  switch it on and tap RESET, then check again.
* `FOC: chip up ... IDLE ... VM 12.x V ... faults none`: the motor chip answers and is set up. `chip not answering`:
  12 V off, or the board has a problem: report it.
* `EEPROM not the board profile` is expected on a new chip (the knob fallback isn't saved yet: rule 4).
* `router "" ... hotspot on 192.168.4.1`: no Wi-Fi saved on this ESP. Tell Cody (he joins the **MaxxFan** hotspot,
  password `maxxfan123` unless changed, and saves Starlink on http://192.168.4.1). Don't do it for him.

### 6. Report to Cody
The two status lines; the firmware, fan mode, motor chip state, VM and faults; the Wi-Fi state and the address of the fan
page (`http://maxxfan.local`, or the IP in the status line). Then point him to the first spin: `maxxfan/README.md`
section 9, "First run". He runs it from the phone page.

### 7. Only if Cody asks you to run the first spin with him, standing at the fan
The laptop must be on the same Wi-Fi as the ESP. `BASE` = `http://maxxfan.local` (or the IP).
1. `GET $BASE/api/status`: `fan_mode` 3, `foc.up` true, `foc.state` `IDLE`, `foc.fault` "". Note `pos` (lid %).
2. If the lid isn't open (`pos` < 95): say so and wait for his go. Then `GET $BASE/api/lid?cmd=open`. It runs for the
   calibrated time (`travel` + `over` ms). If the lid has never been calibrated (README section 5), stop here: he does
   that first.
3. On his go: `GET $BASE/api/fan?rpm=400&dir=fwd`. Poll `/api/status` every second for 20 s and log `foc.state`,
   `foc.rpm`, `m_hz` x 60, `foc.bus_a`, `foc.phase_a`, `foc.fault`. Expect the state to run `IPD`, `OPEN_LOOP`,
   `CLOSED_LOOP_UNALIGNED`, then `CLOSED_LOOP_ALIGNED`, and the rpm to settle near 400.
4. **Stop** with `GET $BASE/api/fan?speed=0` at once if a fault appears, if `bus_a` goes over 3 A, or if Cody says stop.
   Otherwise stop after the 20 s too.
5. Ask him which way the air went (it should be blowing out). If it blew in, the fix is two motor wires swapped in J2,
   with the 12 V off. He does that.
6. Higher speeds (600 to 1200 rpm) only one at a time, each on his go, the same way.

## Updating to a newer build (v4 is already on the ESP)

v4 and later take updates over Wi-Fi, so no USB is needed. They keep their settings, and they roll back by themselves if
the new firmware never answers a web request.
1. `git pull` on `maxxfan-v4-foc`, then build as in step 3 (the version is `FW_VERSION` near the top of
   `maxxfan/maxxfan.ino`).
2. Upload over Wi-Fi with espota (`maxxfan/README.md` section 7). It needs the setup password: ask Cody (`maxxfan-ota` if
   he never changed it). Windows:
   ```
   "%LOCALAPPDATA%\Arduino15\packages\esp32\hardware\esp32\3.3.12\tools\espota.exe" -i maxxfan.local -a <setup password> -f build\maxxfan\maxxfan.ino.bin -r
   ```
   macOS / Linux: `python3 ~/Library/Arduino15/packages/esp32/hardware/esp32/3.3.12/tools/espota.py ...` (Linux:
   `~/.arduino15/...`), same arguments. Or USB as in step 4: that works too, and keeps the settings.
3. Within 10 minutes, `curl http://maxxfan.local/api/status`: that keeps the new firmware (`"fw":"v4.1 ..."`). If it never
   answers, the ESP goes back to the old one by itself.
4. Report the `fw` value, `fan_mode`, and `foc.up` / `foc.state` / `foc.fault`. Same rules as above: nothing moves without
   Cody's go.

## If something's off

| what you see | do |
|---|---|
| compile error | stop and report it, with the error text |
| no 303A port | a charge-only cable, or the SuperMini isn't seated; ask Cody |
| upload keeps failing | the BOOT / RESET dance (step 4); a different USB port or cable |
| `fan mode` not 3 after RESET with 12 V on | report it: the ESP can't see the motor chip on G0 / G1 |
| `FOC: ... faults ...` before anything ran | report the fault names; `GET /api/foc?cmd=clear` clears them, but report first |
| "new firmware on trial" in the serial output | only after a Wi-Fi update (it rolls back unless a page loads within 10 min); a USB upload never is |

Faults during a spin are explained in `maxxfan/README.md` section 9. To stop everything at once: Cody switches off the
12 V.
