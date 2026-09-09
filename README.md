# ESPHome Multi-Device Firmware Repository

> **This is a security-hardened fork of [smlight-tech/slzb-esphome](https://github.com/smlight-tech/slzb-esphome).**
> Branch: [`secure-native-api`](https://github.com/dennisvo/slzb-esphome/tree/secure-native-api).
> The radio UARTs (Zigbee / Thread / Z-Wave) are no longer exposed as plaintext TCP ports on the LAN.
> They are proxied over an **encrypted ESPHome Native API** to Home Assistant (`serial_proxy` + `esphome-hass://` URLs).
> See [Fork Differences](#fork-differences) below and [`docs/design.md`](docs/design.md) for the full rationale.

---

## What is this?

This is an **alternative firmware** for the SMLIGHT network-attached coordinator family (SLZB-MR4U and the other supported boards listed below). It is a full replacement for the stock **SLZB-OS** firmware that ships on the device.

It is built on [ESPHome](https://esphome.io/) and is designed to be paired with [Home Assistant](https://www.home-assistant.io/): you flash it onto your coordinator once, adopt the device through HA's ESPHome integration, and point ZHA / OpenThread Border Router / Z-Wave JS at it over a single encrypted transport.

## Why this fork exists

The stock **SLZB-OS** firmware works, but its network security posture is weak for a device that terminates an entire home's Zigbee and Thread networks:

- The Zigbee, Thread, and USB pass-through UARTs are exposed as **plain, unencrypted TCP sockets** (e.g. `:6638`, `:7638`, `:8638`). Anything on the same LAN that can reach those ports can sniff, inject, disrupt, or take over the radio's serial stream — including driving it into bootloader mode.
- The only access control on those streams is an optional source-IP allow-list, which is **off by default**.
- The management web UI runs on **plain HTTP** (`:80`, no TLS). The admin password — which gates changing radio modes, flashing IEEE addresses, entering the radio bootloader, configuring VPN, etc. — crosses the LAN in cleartext on every login, and so do any session cookies.
- There is no application-level authentication of the coordinator ↔ Home Assistant path at all. Firewalling can hide ports but does not make the underlying protocol trustworthy.

This fork replaces the transport with the **encrypted [ESPHome Native API](https://esphome.io/components/api.html)** (Noise `NNpsk0` + ChaCha20-Poly1305, pre-shared key), removes every plaintext TCP radio port from the network entirely, and closes the plain-HTTP admin surface by not shipping one. See [`docs/design.md`](docs/design.md) for the full threat model and design rationale.

## What you get

- Full functionality of your coordinator: **Zigbee (ZHA), Thread (OpenThread Border Router), Z-Wave JS**, and optional USB pass-through — all reachable from Home Assistant over one encrypted connection.
- Home Assistant device entities for LEDs, buttons, buzzer / RTTTL, WS2812 effects, IR TX/RX, microphone sound level, and PoE / UPS / 4G-addon status — depending on which board you flash.
- Firmware updates over **password-protected OTA** through the ESPHome dashboard.
- Support for multiple boards from a single, structured codebase (see [Supported Devices](#supported-devices)): **ULTIMA**, **MRxU**, **06xU**, **SLWF-09U**.

## Advantages over SLZB-OS

| | SLZB-OS | This firmware |
|---|---|---|
| Radio UART transport | Plaintext TCP `stream_server` | Encrypted ESPHome Native API (`serial_proxy`) |
| Access control on radio streams | Optional source-IP allow-list, off by default | Pre-shared key required (Noise `NNpsk0` + ChaCha20-Poly1305) |
| Management surface | Plain HTTP on `:80`, cleartext admin password | No HTTP admin surface — device is managed through the ESPHome / HA integration |
| OTA firmware update | Unauthenticated | Password-protected |
| Radio reset / bootloader entry | Manual HA switches wired to GPIO | Automatic — the flasher's DTR/RTS are proxied to `nRESET` / `BOOT` |
| Network ports exposed on the LAN | `:80`, `:6638`, `:7638`, `:8638`, … | Only `:6053` (ESPHome Native API, encrypted) |
| Home Assistant integration | Per-radio `socket://ip:port` config | Adopted as a normal ESPHome device; radios addressed via `esphome-hass://…` URLs |
| Configuration model | Vendor-managed image | Open ESPHome YAML — you can add sensors, buttons, automations, effects, etc. |

## Trade-offs and downsides

Being honest about what you give up compared to running the stock firmware:

- **Home Assistant is effectively required.** The Native API transport is designed around the HA ESPHome integration and the `esphome-hass://` URL scheme. If you want to run a coordinator standalone (no HA, or with a non-HA host such as Zigbee2MQTT on bare Linux talking to `socket://`), this firmware is not the right choice — stick with the stock SLZB-OS TCP model.
- **Recent HA versions are required.** You need a Home Assistant version whose ESPHome integration supports `serial_proxy`, and ZHA / OTBR / Z-Wave JS versions that accept the `esphome-hass://` URL scheme.
- **No built-in web admin UI.** SLZB-OS's HTTP dashboard (device info, radio mode switching, VPN config, etc.) is gone by design. Configuration lives in YAML and is applied by re-flashing; runtime state is exposed as normal HA entities.
- **You build and flash the firmware yourself.** No pre-built binaries are published here; you compile with the ESPHome CLI or dashboard against this repo. This is the normal ESPHome workflow but is a shift from downloading a signed vendor image.
- **SLZB-OS-only features are not reproduced.** Vendor extras such as the built-in ZeroTier / WireGuard clients and the SMLIGHT cloud portal are not part of this firmware.
- **Vendor support / warranty caveats.** Custom firmware is not supported by SMLIGHT. Recovery to the stock image is possible via the ESP32-S3 USB bootloader, but you take on that responsibility.

## Choose this firmware if…

- You run Home Assistant and want your Zigbee / Thread / Z-Wave coordinator to stop broadcasting a plaintext serial port on your LAN.
- You already treat the coordinator as "one more ESPHome node" and want to configure it like the rest of your ESPHome fleet.
- You are comfortable building and flashing ESPHome firmware.

## Stick with SLZB-OS if…

- You need the vendor web UI, cloud portal, or the built-in VPN clients.
- You don't run Home Assistant, or your host software cannot use `esphome-hass://` URLs.
- You want vendor support and a signed vendor firmware image.

---

This repository contains a structured ESPHome project designed to support multiple devices and hardware revisions from a single, maintainable codebase.
The architecture emphasizes clear separation between hardware definitions, low-level hardware handling, reusable logic, and device composition.

---

## Fork Differences

### Upstream (`smlight-tech/slzb-esphome`)

The radio UARTs are exposed to the network via [`oxan/esphome-stream-server`](https://github.com/oxan/esphome-stream-server) — one plaintext TCP port per radio (typically `6638`, `6640`, `6641`). Home Assistant integrations (ZHA, OpenThread Border Router, Z-Wave JS) connect to `socket://<device-ip>:<port>`. Anyone on the same L2 segment can read/write the raw coordinator UART.

### This fork (`secure-native-api`)

| Concern | Upstream | This fork |
|---|---|---|
| Radio UART transport | Plaintext TCP (`stream_server`) | Encrypted [ESPHome Native API](https://esphome.io/components/api.html) via [`serial_proxy`](https://esphome.io/components/serial_proxy.html) |
| HA-side URL | `socket://<ip>:<port>` | `esphome-hass://esphome/{entry_id}?port_name=<zigbee\|thread\|zwave>` |
| Auth | None (open TCP) | Pre-shared `api_encryption_key` (Noise / ChaCha20-Poly1305) |
| Radio reset / bootloader entry | HA switches writing GPIO | Automatic — `serial_proxy` drives `dtr_pin` (nRESET) and `rts_pin` (bootloader) from the client's DTR/RTS modem-control signals (matches `zigpy-znp`, `universal-silabs-flasher`, `bellows`, `zwave-js`) |
| OTA | Unauthenticated | Password-protected (`ota_password`) |
| USB pass-through (`packages/usb/usb_uart.yaml`) | Plaintext TCP `:9638` | `serial_proxy` (port name `usb`) — no plaintext port even for the future USB-host variant |

### What is preserved

- All hardware definitions, HAL packages, LEDs / buttons / buzzer / IR / WS2812 / microphone logic.
- All supported devices (ULTIMA, MRxU, 06xU, SLWF-09U).
- The device-composition-driven build model.

### What is removed

- `packages/stream_servers/` (whole directory)
- `packages/external_components/stream_server.yaml` — `serial_proxy` is a first-class ESPHome component, no external source needed
- `packages/buses/uarts/uart_ctrl/` (whole directory) — the per-radio `RST` / `FLASH` GPIO-switch wrappers. Their function is now performed automatically by `serial_proxy` on behalf of the connected client.

### HA-side entity changes

Per-radio, the following Home Assistant entities are **no longer created**:

- `<friendly> <radio> RST` switch
- `<friendly> <radio> FLASH` switch
- `<friendly> <radio> TCP Connected` binary_sensor

Manual radio reset from the HA dashboard is not required in normal operation — the flasher / integration handles DTR/RTS itself over the Native API.

### Setup

1. Copy `secrets.example.yaml` → `secrets.yaml` and fill in:
   - `api_encryption_key` — generate one at <https://esphome.io/components/api.html> (or via `openssl rand -base64 32`).
   - `ota_password` — any strong secret.
   - `wifi_ssid` / `wifi_password` — if not using Ethernet only.
2. Build and flash a device YAML (e.g. `mr4u-r1-73.yaml`) with ESPHome as usual.
3. In Home Assistant, add the device via **ESPHome integration** using the same `api_encryption_key`.
4. In ZHA / OTBR / Z-Wave JS, use the URL:
   `esphome-hass://esphome/{entry_id}?port_name=zigbee` (or `thread` / `zwave` / `usb`).

`{entry_id}` is the ESPHome config-entry id — visible in HA under **Settings → Devices & Services → ESPHome → (your device)**.

---

## Project Structure Overview

```
.
├── devices/      # Device composition files (select hardware defs, HAL, logic)
├── hw_defs/      # Hardware definitions (pins, inversion, revision-specific constants)
├── hal/          # Hardware Abstraction Layer (buses, expanders, low-level wiring)
├── logic/        # Reusable logic blocks (features, actions, controls, sensors)
├── libraries/    # Shared libraries used by logic (e.g. IR, protocol helpers)
├── platform/     # Core platform setup (wifi, diagnostics, bluetooth, external components)
├── docs/         # Architecture and design documentation
└── *.yaml        # Root entry YAMLs per device (build/flash targets)
```

---

## Directory Responsibilities

### devices/
Defines how a specific device firmware is assembled.
Each file:
- Selects a hardware definition from `hw_defs/`
- Includes required HAL buses and expanders
- Enables logic blocks and actions

No low-level pin definitions should live here.

---

### hw_defs/
Pure hardware description layer:
- GPIO pin assignments
- Logic-level inversion (active-low / active-high)
- Board revision and model constants
- Presence flags for optional peripherals

Purpose: keep all hardware differences isolated and readable.

---

### hal/ (Hardware Abstraction Layer)
Contains low-level, hardware-dependent wiring and primitives:
- UART / I2C / SPI buses
- Optional HW flow control variants
- GPIO vs expander-based implementations
- Reset / flash / power control wiring

HAL does not implement business logic.

---

### logic/
Reusable functional and behavioral blocks:
- Features (LEDs, buttons, buzzer, IR, radios, sensors)
- Actions (confirmation, warning, error)
- Control logic that may combine multiple features

Logic is hardware-agnostic and relies on IDs/interfaces provided by HAL.

---

### libraries/
Shared helper libraries used by logic blocks.
Typically protocol-level or domain-specific helpers that are reused across multiple logic modules.

---

### platform/
Common platform configuration shared by all devices:
- ESPHome core configuration
- Wi-Fi / networking
- Diagnostics
- Bluetooth
- External components

---

### docs/
Additional documentation describing architectural decisions and design rules.

---

## Adding a New Device

1. Create or reuse a hardware definition in `hw_defs/`
2. Add a new device composition file in `devices/`
3. Include required HAL buses and logic blocks
4. Build/flash using the corresponding root YAML file

No changes to existing logic or HAL should be required.

---

## Design Goals

- Support many devices and hardware revisions
- Single source of truth for hardware definitions
- Clear separation of concerns
- Minimal duplication
- Easy long-term maintenance and extension

---

## Supported Devices

| Feature | ULTIMA | MRxU | 06xU | SLWF-09U |
|---------|:------------:|:----------:|:----------:|:------------:|
| **MCU** | ESP32-S3 | ESP32-S3 | ESP32-S3 | ESP32-S3 |
| **Flash** | 16MB | 16MB | 16MB | 16MB |
| **PSRAM** | Yes | Yes | Yes | Yes |
| **GPIO LEDs** | 2 | 2 | 2 | 1 |
| **WS2812 RGB** | 12 LEDs | - | - | Yes |
| **Buttons** | 2 | 1 | 1 | 2 |
| **Ethernet** | Yes | Yes | Yes | Yes |
| **UART Radios** | 3 (CC26, EFR32, ZW-800) | 2 (CC26, EFR32) | 1 (CC26/EFR32) | - |
| **Buzzer** | Yes | - | - | - |
| **IR TX** | Yes | - | - | - |
| **IR RX** | Yes | - | - | - |
| **Microphone** | Yes (I2S) | - | - | Yes (I2S) |
| **UPS I2C** | Yes | - | - | - |
| **I2C Expander** | Yes | - | - | - |
| **4G/LTE Addon** | Yes | - | - | - |
| **USB-C CC ADC** | - | Yes | Yes | - |
| **DIY Expansion** | Yes | - | - | Yes |

---

## Usage Examples

### Buzzer / RTTTL Melodies

Devices with a buzzer (e.g., Ultima) support RTTTL melody playback. You can play melodies from Home Assistant.

**Play a custom RTTTL melody:**
```yaml
service: esphome.<device_name>_rtttl_input_set
data:
  value: "mario:d=4,o=5,b=100:16e6,16e6,32p,8e6,16c6,8e6,8g6,8p,8g"
```

**Play a preset melody:**
```yaml
service: esphome.<device_name>_rtttl_preset_set
data:
  option: "Doorbell"
```

Available presets: `Doorbell`, `Notification`, `Alert`, `Success`, `Error`, `Mario`, `Zelda`, `Pacman`, `Star Wars`, `Nokia`

**RTTTL Format:**
```
name:d=duration,o=octave,b=bpm:notes
```

**Resources for RTTTL melodies:**
- [PICAXE RTTTL Collection](https://picaxe.com/rtttl-ringtones-for-tune-command/)
- [Online RTTTL Player/Editor](https://adamonsoon.github.io/rtttl-play/)

---

### WS2812 LED Effects

Devices with WS2812 LEDs (e.g., Ultima) support various light effects controllable from Home Assistant.

**RMT symbol buffer (advanced):**

The WS2812 driver uses the ESP32-S3 RMT peripheral. The TX symbol pool is shared with the IR transmitter (192 symbols total across 4 channels). The default allocation is 96 symbols for WS2812 and 96 for IR TX. If you are not using IR and want to allocate more symbols to WS2812, override the substitution in your device YAML:

```yaml
substitutions:
  ws2812_rmt_symbols: "192"   # increase only if IR TX is disabled
  ir_tx_rmt_symbols: "0"      # set to 0 if not used
```

**Turn on with effect:**
```yaml
service: light.turn_on
target:
  entity_id: light.<device_name>_ws2812
data:
  effect: "Rainbow"
```

**Available effects:**

| Category | Effects |
|----------|---------|
| Common | Rainbow, Color Wipe, Scan, Twinkle, Random Twinkle, Fireworks, Flicker, Pulse, Strobe |
| Lambda | Fire, FastLED Fire, Confetti, Candy Cane, Meteor, Running Lights, Breathing RGB, Color Chase, Sparkle, Christmas |
| Music Reactive | Music: Grav, Music: Gravicenter, Music: Pixels, Music: DJ Light, Music: Waterfall, and more |

**Quick presets via dropdown:**
```yaml
service: esphome.<device_name>_ws2812_preset_set
data:
  option: "Rainbow"
```

Available presets:

| Category | Presets |
|----------|---------|
| Solid Colors | White, Warm White, Red, Green, Blue, Purple, Cyan, Orange |
| Moods | Night Light, Cozy |
| Effects | Rainbow, Fire, Twinkle, Confetti, Party, Christmas |
| Alerts | Alert |

**Note:** Music reactive effects require the microphone to be enabled via the "Mic Enabled" switch.

**Short notification blinks (status indicators):**
```yaml
service: esphome.<device_name>_ws2812_notify_set
data:
  option: "OK"
```

| Notification | Color | Pattern |
|--------------|-------|---------|
| OK | Green | Double blink |
| Warning | Orange | Triple blink |
| Error | Red | Rapid 5x blink |
| Info | Blue | Single long blink |
| Busy | Yellow | Fade out |
| Ready | Cyan | Pulse up then off |
| Attention | Magenta | Double flash |
| Boot | White | Sweep fade |

---

### Microphone / Sound Level

Devices with a microphone (e.g., Ultima) expose sound level sensors.

**Enable microphone:**
```yaml
service: switch.turn_on
target:
  entity_id: switch.<device_name>_mic_enabled
```

**Sensors available:**
- `sensor.<device_name>_mic_rms` - RMS sound level
- `sensor.<device_name>_mic_peak` - Peak sound level

**Warning:** The microphone consumes a lot of CPU and memory resources. It is not recommended to use it simultaneously with Zigbee/Thread/Z-Wave UART-to-Ethernet connections, as it may cause instability or packet loss on those interfaces.

---

### IR Remote (Transmit)

Devices with IR transmitter can send IR codes to control TVs, ACs, etc.

**Send a raw IR code:**
```yaml
service: esphome.<device_name>_ir_send
data:
  code: "0x20DF10EF"  # Example: LG TV Power
```

Check `libraries/ir/codes/` for available IR code packs.

**RMT symbol buffer (advanced):**

The IR transmitter and WS2812 share the ESP32-S3 RMT TX symbol pool (192 symbols total). Defaults are 96 each. Override in your device YAML if needed:

```yaml
substitutions:
  ir_tx_rmt_symbols: "128"    # increase if WS2812 is not used
  ir_rx_rmt_symbols: "96"     # RX pool is independent (192 symbols total)
  ws2812_rmt_symbols: "64"    # reduce if giving more to IR TX
```
