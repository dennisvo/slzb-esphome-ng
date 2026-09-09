# ESPHome Multi-Device Firmware Repository

> **This is a security-hardened fork of [smlight-tech/slzb-esphome](https://github.com/smlight-tech/slzb-esphome).**
> Repository: [`dennisvo/slzb-esphome-ng`](https://github.com/dennisvo/slzb-esphome-ng) (default branch: `main`).
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

## How this firmware compares

Compared against the two SMLIGHT-supported firmwares — proprietary SLZB-OS and their upstream ESPHome build ([`smlight-tech/slzb-esphome`](https://github.com/smlight-tech/slzb-esphome)):

| | SLZB-OS | Upstream ESPHome (`smlight-tech`) | This firmware |
|---|---|---|---|
| Radio UART transport | Plaintext TCP `stream_server` | Plaintext TCP `stream_server` | Encrypted ESPHome Native API (`serial_proxy`) |
| Access control on radio streams | Optional source-IP allow-list, off by default | None — open TCP | Pre-shared key required (Noise `NNpsk0` + ChaCha20-Poly1305) |
| ESPHome Native API encryption | n/a | Off by default (plaintext) | Pre-shared key required |
| Management surface | Plain HTTP on `:80`, cleartext admin password | No HTTP admin — managed via ESPHome / HA | No HTTP admin surface — device is managed through the ESPHome / HA integration |
| OTA firmware update | Unauthenticated | Unauthenticated by default | Password-protected |
| Radio reset / bootloader entry | Manual HA switches wired to GPIO | Manual HA switches wired to GPIO | Automatic — the flasher's DTR/RTS are proxied to `nRESET` / `BOOT` |
| Network ports exposed on the LAN | `:80`, `:6638`, `:7638`, `:8638`, … | `:6053` (plaintext API) plus `:6638`, `:7638`, `:8638` (plaintext `stream_server`) | Only `:6053` (ESPHome Native API, encrypted) |
| Home Assistant integration | Per-radio `socket://ip:port` config | ESPHome device in HA, but radios still consumed via `socket://ip:port` | Adopted as a normal ESPHome device; radios addressed via `esphome-hass://…` URLs |
| Configuration model | Vendor-managed image | Open ESPHome YAML — extensible with sensors, buttons, automations, effects, etc. | Open ESPHome YAML — extensible with sensors, buttons, automations, effects, etc. |

## Trade-offs and downsides

Being honest about what you give up compared to running the stock firmware (SLZB-OS):

- **Home Assistant is effectively required.** The Native API transport is designed around the HA ESPHome integration and the `esphome-hass://` URL scheme. If you want to run a coordinator standalone (no HA, or with a non-HA host such as Zigbee2MQTT on bare Linux talking to `socket://`), this firmware is not the right choice — stick with the stock SLZB-OS TCP model.
- **Recent HA versions are required.** You need a Home Assistant version whose ESPHome integration supports `serial_proxy`, and ZHA / OTBR / Z-Wave JS versions that accept the `esphome-hass://` URL scheme.
- **No built-in web admin UI.** SLZB-OS's HTTP dashboard (device info, radio mode switching, VPN config, etc.) is gone by design. Configuration lives in YAML and is applied by re-flashing; runtime state is exposed as normal HA entities.
- **You build and flash the firmware yourself.** No pre-built binaries are published here; you compile with the ESPHome CLI or dashboard against this repo. This is the normal ESPHome workflow but is a shift from downloading a signed vendor image.
- **SLZB-OS-only features are not reproduced.** Vendor extras such as the built-in ZeroTier / WireGuard clients and the SMLIGHT cloud portal are not part of this firmware.
- **Third-party fork, not SMLIGHT-supported.** SMLIGHT ships two supported firmware paths for the SLZB adapters: their proprietary SLZB-OS and their own upstream ESPHome build ([smlight-tech/slzb-esphome](https://github.com/smlight-tech/slzb-esphome)). This project is a security-hardened fork of the latter and is not the SMLIGHT-supported build. Before contacting SMLIGHT for anything hardware-related, reflash to one of the supported firmwares first (e.g. via the [SMLIGHT web flasher](https://smlight.tech/flasher/) or USB) so the conversation is about the hardware, not this fork.

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

### This fork (`slzb-esphome-ng`)

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

Two supported workflows depending on how much you want to customize the firmware.

#### Which one should I pick?

| I want to… | Use |
|---|---|
| Just flash the firmware, minimal fuss, get updates by bumping a git ref | **Workflow A — thin importable file** |
| Customize the firmware, add sensors, tweak logic, hack on it | **Workflow B — clone the full repo** |

Both workflows share the same `secrets.yaml` step below.

#### Shared step — `secrets.yaml`

Create `/config/esphome/secrets.yaml` (HA add-on) or `secrets.yaml` in your ESPHome working directory (CLI) with:

```yaml
api_encryption_key: "<base64 32-byte key>"    # openssl rand -base64 32
ota_password: "<any strong secret>"
# wifi_ssid: "..."                            # only if not Ethernet-only
# wifi_password: "..."
```

A template is provided as [`secrets.example.yaml`](secrets.example.yaml).

#### Workflow A — thin importable file (recommended for most users)

The [`importable/`](importable/) directory contains 5 tiny files (~15 lines each). Each one uses ESPHome's [remote-package feature](https://esphome.io/components/packages.html) to pull the full device composition from this GitHub repo at build time.

1. In the ESPHome dashboard, click **+ New device** → give it a name → skip the "install" step at the end.
2. Open the generated YAML in the dashboard editor and **replace its contents** with the appropriate file from [`importable/`](importable/):

   | Board | Copy from |
   |---|---|
   | SLZB-MR4U r1.73 | [`importable/slzb-mr4u.yaml`](importable/slzb-mr4u.yaml) |
   | SLZB-MRxU r1.73 | [`importable/slzb-mrxu.yaml`](importable/slzb-mrxu.yaml) |
   | SLZB-06 / 07 (`06xU`) r1.73 | [`importable/slzb-06xu.yaml`](importable/slzb-06xu.yaml) |
   | Ultima r1.04 | [`importable/slzb-ultima.yaml`](importable/slzb-ultima.yaml) |
   | SLWF-09U r1.01 | [`importable/slwf-09u.yaml`](importable/slwf-09u.yaml) |

3. **Install → Manual download** (first flash, requires USB), or **Install → Wirelessly** if you're upgrading from an earlier build of this firmware.

Bump the `ref:` inside the file to a tagged release (or a specific commit SHA) to pin a stable version. Leave it on `main` to always track latest.

#### Workflow B — clone the full repo (for developers / customizers)

Clone the whole tree into your ESPHome config directory:

- **ESPHome dashboard (HA add-on):** clone into `/homeassistant/esphome/` on HA OS (or `/config/esphome/` on Supervised). The dashboard picks up the root `*.yaml` files automatically:

   | Board | Build target |
   |---|---|
   | SLZB-MR4U r1.73 | `mr4u-r1-73.yaml` |
   | SLZB-MRxU r1.73 | `mrxu-r1-73.yaml` |
   | SLZB-06 / 07 | `06xu-r1-73.yaml` |
   | Ultima r1.04 | `ultima-r1-04.yaml` |
   | SLWF-09U r1.01 | `slw09u-r1-01.yaml` |

- **ESPHome CLI:**

  ```bash
  git clone https://github.com/dennisvo/slzb-esphome-ng.git
  cd slzb-esphome-ng
  esphome run mr4u-r1-73.yaml       # first-time flash over USB
  esphome upload mr4u-r1-73.yaml    # subsequent OTA updates
  ```

Each root file is a one-line `!include devices/*.yaml`; the full tree (`devices/`, `packages/`, `hw_defs/`, `libraries/`, `components/`) must be present alongside it.

#### After the first install (both workflows)

1. In Home Assistant, add the device via **ESPHome integration** using the same `api_encryption_key`.
2. In ZHA / OTBR / Z-Wave JS, use the URL:
   `esphome-hass://esphome/{entry_id}?port_name=zigbee` (or `thread` / `zwave` / `usb`).

   `{entry_id}` is the ESPHome config-entry id — visible under **Settings → Devices & Services → ESPHome → (your device)**.

---

## Project structure

See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the full layout, layer responsibilities, pin-abstraction rules, and instructions for adding a new device.

Short version: `mr4u-r1-73.yaml` (and its four siblings) are one-line entry points that `!include devices/*.yaml`. Devices compose `packages/*.yaml`, which are parameterized by substitutions defined once in `hw_defs/`.

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
