# Van hub (ESP32-C3)

The van's dashboard: battery, solar, water and the MaxxFan on one page at **http://van.local**. A second
**ESP32-C3 SuperMini (HW-466AB)**, the other one of the 2-pack, replaces the Arduino Uno on the flow meter.

Firmware: `hub.ino` **v1**, **compiled, not flashed yet**. Arduino core "esp32" by Espressif 3.3.12 + **NimBLE-Arduino
2.5.1**; board settings in `sketch.yaml` (same as the MaxxFan). 1.38 MB = 70 % of the 1.9 MB app slot. Wi-Fi, updates and
passwords come from `../libraries/vannet`, the same code the MaxxFan runs.

| what | how | needs |
|---|---|---|
| Solar: 200W Aft (SmartSolar MPPT 100/30), 320W Fwd (SmartSolar MPPT 75/15) | Victron **Instant Readout**: encrypted Bluetooth advertisements, no connection, no wiring | each device's key, pasted on the setup page |
| Battery: Jimbo (SmartShunt 500A/50mV) | Instant Readout, as above: state of charge, V, A, time left | its key |
| Battery: Renogy RBT200LFP12-BT (Bluetooth name **BT-TH-66FE625A**) | Bluetooth connection every 30 s, reads the BMS (Modbus): V, A, Ah left, cells, temperatures | nothing (name is the default) |
| Water | flow meter pulses on **G10** | the wiring below |
| MaxxFan | its HTTP API (`../maxxfan/README.md`, section 8) | both on the van router |

## 1. Wiring

Build card with drawings, parts, steps and bring-up: **`build_card.html`** (open it in a browser, print it).

**The box** (2026-10-12): `S:\aiCAD\van_hub_box`, printed in PETG, base + lid on one plate (`hub_box_plate.step`,
15 g, 34 min). It holds the bare ESP (no headers) and the 10k / 20k divider. The sensors **plug and unplug at a JST-SM
3-pin** LED-strip plug just outside it: the hub half of the pigtail leaves through a clamped window and is zip-tied
inside, so the meter half is the meter's three wires and nothing else. Lid: 2 x M3 x 10; mounting: 2 ears or VHB.

| ESP pin | in the box | pigtail -> meter |
|---|---|---|
| USB-C | through the end-wall slot | 12 -> 5 V converter's USB-C lead (as the MaxxFan), converter on a 2 A fuse |
| 5V pin | straight through | red -> flow meter red: the sensor needs 5 V, this is the USB-C 5 V (it draws ~15 mA) |
| GND | the 20k joins it | white -> flow meter black |
| **G10** | divider joint: **20k** to GND, **10k** on to the pigtail | green -> flow meter signal (yellow) |
| G8 / G9 | | on-board LED / BOOT: don't connect. G2 is a boot pin too. |

The ESP wires go into the pad holes from underneath and run along the box floor under the board's edges. The divider
keeps a 5 V signal at 3.3 V. If the meter turns out to be a bare open collector (no flow shown with water running), the
20k holds G10 low: open the lid, snip one leg of it, and the ESP's own pull-up (on in the firmware) does the job. Free
for later: G0, G1, G3, G4 (ADC: a tank level probe, section 5), G5-G7, G20, G21; the window takes a second pigtail.

## 2. First use

Flash over USB as the MaxxFan (`../maxxfan/README.md`, section 4), with `hub` in place of `maxxfan`:

```
arduino-cli compile --libraries libraries --output-dir build/hub hub
arduino-cli upload -p COM5 hub
```

1. With no router saved the hub runs its own hotspot **VanHub** (password `vanhub123`): join it, open
   **http://192.168.4.1**, then **Wi-Fi** at the bottom: scan, pick the Starlink network, password, **save + join**.
2. Back on Starlink: **http://van.local**, **Share > Add to Home Screen**. iPhone Wi-Fi settings: VanHub > Auto-Join off.
3. **http://van.local/setup** (user `van`, password `vanhub-ota`): set your own **passwords** at the bottom.

### Victron keys (once per device)
Instant Readout is already on for all three (VictronConnect shows their numbers in the device list). For each one:

1. VictronConnect > the device > gear icon > **&#8942; > Product info** > **Instant readout details: Show**. Copy the
   **encryption key** with the copy button (32 characters 0-9 / a-f; typing it misses the last character easily).
2. **http://van.local/setup** > Victron: the devices the hub hears are listed with their Bluetooth names. Paste the key
   into the same device, give it a name (200W Aft, 320W Fwd, Jimbo), **Save**. The line turns green: `key OK`.

`wrong key` = it's another device's key. While VictronConnect is **connected** to a device, that device stops sending
Instant Readout; the hub shows the readings as old until the app disconnects.

### Renogy battery
Nothing to set: the hub looks for **BT-TH-66FE625A**, connects every 30 s, reads, disconnects. While the phone's
DC Home app is connected to the battery the hub can't connect (the module takes one connection): the page says so, and
reads again when the app lets go. If the battery never answers, try Modbus address **48** on the setup page.

## 3. The page

* **Battery**: state of charge from the SmartShunt (the Renogy BMS when the shunt is missing), V, A, W, time left;
  the Renogy BMS line under it: its own %, V, A, Ah left of 200, cell range, warmest temperature.
* **Solar**: PV watts and amps into the battery for both chargers together, kWh today; one line per charger with its
  state (bulk / absorption / float) and any error.
* **Water**: gallons left and %, used since the tank was filled, flow while it runs. **Tank filled** starts the count
  from full again (section 5).
* **MaxxFan**: lid and fan as on its own page; open / close / stop, fan speed and direction (when its fan mode is
  Phone or Phone + knob). Hub commands count as phone commands: the knob still takes over when turned.
* Readings over 2 min old are marked as old, e.g. `(5 min old)`.

Setup page: Victron keys and names, Renogy name / address / interval, flow meter pulses per litre, tank size, units
(US or metric), the MaxxFan's address, firmware update, passwords.

## 4. Updates, API, serial

Updates exactly as the MaxxFan (`../maxxfan/README.md`, section 7) with **hub.ino.bin** (GitHub Actions: **hub-firmware**)
on **http://van.local/setup**, or `espota.exe -i van.local -a <setup password> -f build\hub\hub.ino.bin -r`. Same trial:
the new firmware is kept once the dashboard loads, otherwise it rolls back after 10 min. Bluetooth stops during an update.

| request | does |
|---|---|
| `GET /api/status` | everything: `victron` [ ... ], `renogy` { ... }, `water` { ... }, `maxxfan` { ok, s: the MaxxFan's own status }, `wifi_*`, `fw`, `units` |
| `GET /api/maxxfan?lid=open\|close\|stop` / `?speed=0..100&dir=fwd\|rev` | passed on to the MaxxFan |
| `POST /api/water` `filled=1` | tank full: used = 0 |
| `POST /api/wifi`, `GET /api/scan` | van router (as the MaxxFan) |
| `GET /setup`, `POST /api/victron`, `/api/hub`, `/api/setup`, `/update` | setup (login) |

USB serial 115200: `?` prints Wi-Fi, every Victron device with its readings, the Renogy reads, water, MaxxFan.

## 5. Water: what the old Arduino did, and the tank level question

**The old Uno** (read back from its flash, 2026-10-11; no source): the Adafruit "Flow sensor test" sketch plus a
16x2 LCD keypad shield. Flow sensor signal on **D2** with the pull-up on, sampled 1000 times a second; **450 pulses per
litre** (Adafruit's `litres = pulses / 7.5 / 60`, a YF-S201 style 1/2" sensor); tank **25 US gal** (94.635 L). The LCD
showed litres left, % left and litres used; a buzzer on D3 beeped every 10 L; a shield button toggled the backlight. Its
weak points: the count lived in RAM only (and in 16 bits: it wraps after 145 L), so every power-up meant "tank full";
no "filled" button; it never knew about a fill, a leak or a drain.

**The hub now** keeps that sensor and those numbers but stores the count through power cuts and has a **Tank filled**
button. It is still dead reckoning: a cheap hall flow meter is good to about 10 %, worse with the pump's pulsing flow or
air in the line. Calibrate it once: fill a 1 gal jug from a tap, compare, and scale pulses per litre on the setup page.

**Measuring the level instead** (to decide):

| option | what | for | against |
|---|---|---|---|
| A. flow meter only (as now) | count down from "filled" | no new hardware | an estimate; needs the button at every fill |
| B. submersible pressure sensor | 4-20 mA level probe (0-0.5 or 0-1 m water) on the tank floor, wire out through the fill / vent or a top fitting; 12 V, 100 R to G4 | the real level, continuous, sees fills / leaks; ~0.5 % | needs a way in at the top; 12 V supply |
| C. stick-on level switches | capacitive sensors (XKC-Y25 type) taped to the outside of a plastic tank at 3-4 heights | nothing in the water, cheap | steps only (25 / 50 / 75 / full) |
| B or C + the flow meter | level for the truth, flow for gallons per day and a running total between the steps | both | more wiring |

The tank is **plastic (probably PE)**, so C works through its wall. Still needed: its shape and height, how you get to
its top (fill and vent fittings), and whether 25 gal is right.

**A probe in drinking water** (2026-10-12): fine with the right materials. Buy one with a **316L stainless** body and
diaphragm and a **PE cable** (many cheap ones offer PVC / PU / PE: take PE, it is the cable that sits in the water). Range
0-0.5 m or 0-1 m water, 4-20 mA, supply minimum **9 V or lower** (the van runs 12-14.4 V and the 100 R takes up to 2 V).
Certified ones (NSF/ANSI 61) exist, e.g. KELLER Levelgage (PE jacket only) and AMETEK PMT 575, at several times the price and
with ranges meant for wells. Watch: **freezing** (ice can crush the diaphragm; most cheap cells have a few drops of
silicone oil behind it): drain the tank or lift the probe out for freezing storage. The cable's **vent tube** must end in dry
open air (inside the hub box), never pinched or sealed. Seal the cable where it enters the tank (gland or grommet: PE
can't be glued). Normal bleach sanitizing doesn't hurt 316L or PE.

## 6. This van's Victron setup (VictronConnect export, 2026-10-11)

* **200W Aft**: SmartSolar MPPT 100/30, HQ1949EKDC8, firmware v3.60 (v3.72 available). Absorption 14.2 V, float
  14.2 V, absorption max 2 h, user-defined battery type, low-temperature charging cut at 5 C (but no temperature source on
  the network).
* **320W Fwd**: SmartSolar MPPT 75/15, HQ1912IQPVW, firmware v3.60.
* **Jimbo**: SmartShunt 500A/50mV.
* All three are in the VE.Smart network "Inde"; Instant Readout works alongside it.
