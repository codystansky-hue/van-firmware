# MaxxFan controller (ESP32-C3)

Lid motor and fan control for the MaxxFan BLDC retrofit. The mechanical / CAD side (boxes, harnesses, the aqx BLDC
driver, the 74AHCT125 buffer) lives in the aiCAD project, `S:\aiCAD\maxxfan_bldc` (`HARNESS_BOM.md`, `WIRING.md`,
`DESIGN.md`).

Board: **ESP32-C3 SuperMini (HW-466AB)**, 22.5 x 18 mm, USB-C, headers soldered.
Firmware: `maxxfan.ino` **v3**. Arduino core "esp32" by Espressif 3.3.12; board settings in `sketch.yaml`.
1.12 MB = 57 % of the 1.9 MB app slot.

What it does:
* **Lid** (DRV8871): open / close / stop / nudge from the phone page, the BOOT button, an optional panel button, or USB
  serial. Timed moves (no position sensor), calibrated on the page.
* **Fan** (aqx BLDC board, through the buffer): speed, exhaust / intake, from the phone **and** the knob + switch;
  whichever you touched last wins. Only runs with the lid open.
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

In **Phone + knob**:
* Turning the knob (5 % or more) or flipping the direction switch hands the fan to the panel; it then follows the knob.
* A phone (or hub) command hands it to the phone until the knob is turned again. The status line shows
  `set by phone, knob at 40%`.
* Closing the lid cancels a phone request. With the knob up, the fan starts again when the lid reopens.
* At power-up the knob position runs the fan only if the lid is already open; the ESP never opens the lid by itself at boot.
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
  If full knob doesn't reach 100 %, or reaches it early, change `KNOB_FULL_MV` in the sketch (now 2150).
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
arduino-cli compile maxxfan
arduino-cli upload  -p COM5 maxxfan
arduino-cli monitor -p COM5 -c baudrate=115200
```
On this PC arduino-cli is `%USERPROFILE%\tools\arduino-cli\arduino-cli.exe`.

### Arduino IDE 2
Install **esp32 by Espressif Systems** 3.x (boards manager URL
`https://espressif.github.io/arduino-esp32/package_esp32_index.json`). Open `maxxfan\maxxfan.ino`. **Tools**: ESP32C3 Dev
Module, USB CDC On Boot **Enabled**, Flash Size 4MB, Partition Scheme **Minimal SPIFFS (1.9MB APP with OTA/128KB SPIFFS)**.
Upload, then Serial Monitor at 115200: `MaxxFan ESP v3 ... ready`.

### If the upload fails ("No serial data received", "Failed to connect")
**Hold BOOT, tap RESET (or plug in USB while holding BOOT), release BOOT**, upload again, tap RESET after it finishes.

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
1. **nudge open** (0.5 s). If it moved the wrong way: tick **swap direction**, **Save settings**.
2. **Calibrate**: nudge fully closed, **Set closed**; press **OPEN** and time it to fully open (**STOP** if it stalls);
   enter that as **full travel ms**, keep **extra ms** ~600, **Save**.
3. **OPEN / CLOSE** then run the calibrated time; **STOP** stops instantly; the position estimate survives power-off.
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
arduino-cli compile --output-dir build/maxxfan maxxfan
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
| `GET /api/status` | JSON: `state`, `pos` (lid %, -1 unknown), `fan_mode` (0 knob only, 1 phone, 2 phone + knob), `fan` (requested %), `fan_out` (running %), `dir` (`fwd` = exhaust / `rev`), `fan_src` (`knob` / `phone`), `fan_wait` (`lid` / `reverse` / ""), `m_hz`, `knob`, `knob_mv`, `wifi_*`, `ap`, `fw` |
| `GET /api/lid?cmd=open\|close\|stop\|jog_open\|jog_close\|set_closed[&ms=500]` | lid |
| `GET /api/fan?speed=0..100&dir=fwd\|rev` | fan, as a phone request (opens the lid if needed) |
| `GET /api/settings?travel=&over=&ramp=&invert=0\|1` | lid calibration |
| `POST /api/wifi` `ssid`, `pass` / `forget=1` | van router |
| `GET /api/scan` | `{"busy":true}` until `{"nets":[...]}` |
| `GET /setup`, `POST /api/setup` `fanmode`, `ap_pass`, `ota_pass` | setup (basic auth), restarts |
| `POST /update` multipart `.bin` | firmware (basic auth) |
| UDP/TCP 3232 | ArduinoOTA / espota, password = setup password |
