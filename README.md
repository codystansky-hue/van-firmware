# van-firmware

ESP32 firmware for the van.

| folder | what | state |
|---|---|---|
| [`maxxfan/`](maxxfan/README.md) | MaxxFan BLDC retrofit: lid motor, fan (phone + knob), Wi-Fi on Starlink, updates over Wi-Fi | v3.1, compiled, not flashed yet |
| [`hub/`](hub/README.md) | van hub: Victron solar + SmartShunt, Renogy battery, water, MaxxFan on one page, http://van.local | v1, compiled, not flashed yet |
| [`libraries/vannet/`](libraries/vannet/src/VanNet.h) | shared by both: Wi-Fi (router first, own hotspot as fallback), updates with rollback, passwords | |

Hardware for both: **ESP32-C3 SuperMini (HW-466AB)**. The MaxxFan's mechanical side (CAD, boxes, harnesses) is in the
aiCAD project, `S:\aiCAD\maxxfan_bldc`.

## Build

Arduino core **esp32 by Espressif 3.3.12** with arduino-cli. Each sketch folder has a `sketch.yaml` with its board
settings (ESP32C3 Dev Module, USB CDC On Boot, Minimal SPIFFS 1.9 MB app with OTA). `--libraries libraries` brings in the
shared `vannet`; the hub also needs NimBLE-Arduino:

```
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli lib install NimBLE-Arduino@2.5.1
arduino-cli compile --libraries libraries --output-dir build/maxxfan maxxfan
arduino-cli compile --libraries libraries --output-dir build/hub hub
```

On Cody's PC arduino-cli is at `%USERPROFILE%\tools\arduino-cli\arduino-cli.exe` with the core and NimBLE installed (and
the AVR core, used to read back the old flow-meter Uno).

**GitHub Actions** builds every push to `main` (`.github/workflows/build.yml`). The firmware files are attached to the
run (**maxxfan-firmware**, **hub-firmware**). Pushing a tag `vN` also makes a release with `maxxfan-vN.bin` and
`hub-vN.bin`. Either one can be downloaded on the phone and uploaded on the device's setup page (`maxxfan/README.md`,
section 7).

## Conventions

* No secrets in git: Wi-Fi and device passwords are set on each device's pages and kept in its flash. The defaults
  in the sketches are only for the first boot.
* Default passwords: MaxxFan hotspot `MaxxFan` / `maxxfan123`, setup login `maxxfan` / `maxxfan-ota`; hub hotspot
  `VanHub` / `vanhub123`, setup login `van` / `vanhub-ota`. Change them on `/setup` after the first boot. The Victron
  Instant Readout keys are pasted on the hub's setup page and kept on the hub too.
* Write the date and what changed at the top of the sketch when bumping a version.
