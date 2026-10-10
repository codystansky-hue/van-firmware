# van-firmware

ESP32 firmware for the van.

| folder | what | state |
|---|---|---|
| [`maxxfan/`](maxxfan/README.md) | MaxxFan BLDC retrofit: lid motor, fan (phone + knob), Wi-Fi on Starlink, updates over Wi-Fi | v3, compiled, not flashed yet |
| [`hub/`](hub/README.md) | van hub: Victron solar, Renogy battery, flow meter, MaxxFan, one dashboard | plan, waiting on the device details |

Hardware for both: **ESP32-C3 SuperMini (HW-466AB)**. The MaxxFan's mechanical side (CAD, boxes, harnesses) is in the
aiCAD project, `S:\aiCAD\maxxfan_bldc`.

## Build

Arduino core **esp32 by Espressif 3.3.12** with arduino-cli. Each sketch folder has a `sketch.yaml` with its board
settings (ESP32C3 Dev Module, USB CDC On Boot, Minimal SPIFFS 1.9 MB app with OTA), so:

```
arduino-cli core install esp32:esp32@3.3.12 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli compile --output-dir build/maxxfan maxxfan
```

On Cody's PC arduino-cli is at `%USERPROFILE%\tools\arduino-cli\arduino-cli.exe` with the core installed.

**GitHub Actions** builds every push to `main` (`.github/workflows/build.yml`). The firmware file is attached to the run
(**maxxfan-firmware**). Pushing a tag `vN` also makes a release with `maxxfan-vN.bin`. Either one can be downloaded on
the phone and uploaded on the device's setup page (`maxxfan/README.md`, section 7).

## Conventions

* No secrets in git: Wi-Fi and device passwords are set on each device's pages and kept in its flash. The defaults
  in the sketches are only for the first boot.
* Default passwords: hotspot `MaxxFan` / `maxxfan123`, setup login `maxxfan` / `maxxfan-ota`. Change them on
  `/setup` after the first boot.
* Write the date and what changed at the top of the sketch when bumping a version.
