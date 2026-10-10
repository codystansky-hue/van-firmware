# Van hub (ESP32-C3) -- plan

Not started. A second **ESP32-C3 SuperMini** (the other one from the 2-pack) that replaces the Arduino on the flow meter
and becomes the van's dashboard: battery, solar, water and the MaxxFan on one page at **http://van.local**.

## Plan

* **Wi-Fi + updates**: the same scheme as the MaxxFan: joins Starlink, hotspot fallback, updates from the phone or a PC
  with rollback, passwords kept on the ESP. Move that code out of `maxxfan.ino` into a shared library
  (`libraries/vannet`) when the hub starts, so both sketches use one copy.
* **Victron solar controllers**:
  * SmartSolar (Bluetooth built in): read the **Instant Readout** broadcasts over BLE. No wiring. Each controller needs
    Instant Readout turned on and its encryption key (VictronConnect > the controller > settings > Product info >
    Instant readout details).
  * BlueSolar (no Bluetooth): VE.Direct cable (19200 baud text protocol, 1 frame a second) to a UART pin. Check the port's
    signal level; a 5 V port needs a divider on its TX.
* **Renogy battery**: over Bluetooth (BT-2 dongle or built-in Bluetooth) or wired RS485 / Modbus through a 3.3 V
  transceiver (MAX3485 module). Depends on the model.
* **Flow meter**: pulse counting on an interrupt pin (as the MaxxFan does with M). A 5 V sensor output needs a divider
  (10k / 20k). Pulses per litre come from the current Arduino sketch.
* **MaxxFan**: read `GET http://maxxfan.local/api/status`, drive `/api/lid` and `/api/fan` (see `../maxxfan/README.md`,
  section 8). Hub commands count as phone commands, so the knob still takes over when turned.

### The ESP32-C3 for this job
* Bluetooth LE + Wi-Fi run together (one radio, time-shared). The 1.9 MB app slot (same partition scheme) leaves room
  for the BLE stack.
* 2 UARTs: USB is the console, so both hardware UARTs are free (VE.Direct, RS485).
* ADC only on G0-G4 (G2 is a boot pin). Free pins depend on what ends up wired.

## Information needed (Cody, in the van)

1. **Victron controllers**: model(s) (SmartSolar / BlueSolar, e.g. "SmartSolar MPPT 100/30") and how many. The Instant
   Readout keys from VictronConnect if they are SmartSolar.
2. **Renogy battery**: model, and how it talks: BT-2 dongle, built-in Bluetooth, or an RS485 / communication port (RJ45?).
3. **Flow meter**: model or a photo of its label, what it measures (fresh water used? tank?), supply voltage, and the
   current Arduino sketch (copy it into this folder).
4. Where the hub mounts and how it gets power (12 -> 5 V USB-C like the MaxxFan?).
