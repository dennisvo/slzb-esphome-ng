# ESPHome Multi-Device Firmware Repository

> **This is a security-hardened fork of [smlight-tech/slzb-esphome](https://github.com/smlight-tech/slzb-esphome).**
> Repository: [`dennisvo/slzb-esphome-ng`](https://github.com/dennisvo/slzb-esphome-ng) (default branch: `main`).
> The radio UARTs (Zigbee / Thread / Z-Wave) are no longer exposed as plaintext TCP ports on the LAN.
> They are proxied over an **encrypted ESPHome Native API** to Home Assistant (`serial_proxy` + `esphome-hass://` URLs).
> See [Fork Differences](#fork-differences) below and [`docs/design.md`](docs/design.md) for the full rationale.

---

## What is this?

This is an **alternative firmware** for SMLIGHT's family of network-attached ESP32 serial devices — the SLZB-… Zigbee / Thread / Z-Wave coordinators (SLZB-MR4U, SLZB-MRxU, SLZB-06/07, SLZB-Ultima) and the SLWF-09U USB-over-network gateway. It is a full replacement for the stock SMLIGHT firmware (**SLZB-OS** on the coordinators, the equivalent stock firmware on the SLWF).

All of these boards share the same underlying problem: they expose one or more serial streams (integrated radios on the SLZB family, whatever USB device is plugged into the SLWF) as plaintext TCP ports on the LAN. This firmware replaces that transport with the encrypted ESPHome Native API.

It is built on [ESPHome](https://esphome.io/) and is designed to be paired with [Home Assistant](https://www.home-assistant.io/): you flash it onto the device once, adopt it through HA's ESPHome integration, and point ZHA / OpenThread Border Router / Z-Wave JS at it over a single encrypted transport.

## Why this fork exists

The stock **SLZB-OS** firmware works, but its network security posture is weak for a device that terminates an entire home's Zigbee and Thread networks:

- The Zigbee, Thread, and USB pass-through UARTs are exposed as **plain, unencrypted TCP sockets** (e.g. `:6638`, `:7638`, `:8638`). Anything on the same LAN that can reach those ports can sniff, inject, disrupt, or take over the radio's serial stream — including driving it into bootloader mode.
- The only access control on those streams is an optional source-IP allow-list, which is **off by default**.
- The management web UI runs on **plain HTTP** (`:80`, no TLS). The admin password — which gates changing radio modes, flashing IEEE addresses, entering the radio bootloader, configuring VPN, etc. — crosses the LAN in cleartext on every login, and so do any session cookies.
- There is no application-level authentication of the coordinator ↔ Home Assistant path at all. Firewalling can hide ports but does not make the underlying protocol trustworthy.

This fork replaces the transport with the **encrypted [ESPHome Native API](https://esphome.io/components/api.html)** (Noise `NNpsk0` + ChaCha20-Poly1305, pre-shared key), removes every plaintext TCP radio port from the network entirely, and closes the plain-HTTP admin surface by not shipping one. See [`docs/design.md`](docs/design.md) for the full threat model and design rationale.

## What you get

- Full functionality of your coordinator: **Zigbee (ZHA), Thread (OpenThread Border Router), Z-Wave JS** — reachable from Home Assistant over one encrypted connection.
- Home Assistant device entities for LEDs, buttons, buzzer / RTTTL, WS2812 effects, IR TX/RX, and PoE / UPS / 4G-addon status — depending on which board you flash.
- Firmware updates over **password-protected OTA** through the ESPHome dashboard.
- **Radio firmware version reported to Home Assistant (v1: CC26xx ZNP only).** Each radio publishes its installed firmware version as a diagnostic sensor. In v1 the **CC26xx ZNP probe is live** (returns the actual firmware version straight from the coordinator). **Spinel (EFR32 Thread), EZSP (EFR32 Zigbee) and Z-Wave sensors ship as stubs** that publish `"unknown (<protocol> probe not implemented in v1)"` until the real probes land in v1.x. A small HA template snippet reads SMLIGHT's public firmware catalog and shows an **"update available"** entity per radio, filtered by the `prod` / `dev` channel declared in your device YAML. Read-only; radio flashing itself is not part of v1.
- Support for multiple boards from a single, structured codebase (see [Supported Devices](#supported-devices)): **ULTIMA**, **MRxU**, **06xU**, **SLWF-09U**.

## Status at a glance

This is a living project. High-level snapshot of where things stand — see [`docs/roadmap.md`](docs/roadmap.md) for the full plan with per-item status, deferred work, and explicit non-goals.

- **Shipped in v1 (today):** encrypted `serial_proxy` transport for every radio UART (no plaintext TCP on the LAN); password-protected OTA; automatic DTR/RTS reset/bootloader entry driven by the flasher; CC26xx ZNP live firmware-version probe published to HA as a diagnostic sensor + optional HA template snippet that compares against SMLIGHT's public catalog for "update available" cards.
- **In flight for v1.x (stubs today, real probes coming):** Spinel / EZSP / Z-Wave firmware-version probes (currently publish `"unknown (<protocol> probe not implemented in v1)"`); catalog-schema follow-ups surfaced by the first snapshot; auto-detect `prod`/`dev` channel from the running revision.
- **Planned for v2:** flash radio firmware end-to-end from Home Assistant (HA add-on drives our ESP32 to reflash the radio over the Native API); HA `select` entities for radio `protocol` / `role` / `channel` that trigger real reflashes when you change them.

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
| Radio firmware version visibility in HA | Vendor web UI shows it; nothing in HA | Not exposed | Boot-time probe per radio, published as diagnostic sensor over the Native API. Live in v1 for CC26xx ZNP only; Spinel / EZSP / Z-Wave are stub sensors reporting `"unknown (… not implemented in v1)"` until v1.x. Optional HA template snippet compares the live sensor against SMLIGHT's public catalog to flag updates for the channel (`prod`/`dev`) declared in device YAML |
| Sound-reactive WS2812 effects (mic-driven) | Vendor implementation (Ultima only) | Enabled by default via third-party [`music_leds`](https://github.com/andrewjswan/esphome-components) / `fastled_helper` (WLED-derived FFT + FastLED pipeline) | Not shipped — SoC CPU / interrupt / timing budget is reserved for the radio UARTs (see [`docs/design.md §18`](docs/design.md)) |
| Configuration model | Vendor-managed image | Open ESPHome YAML — extensible with sensors, buttons, automations, effects, etc. | Open ESPHome YAML — extensible with sensors, buttons, automations, effects, etc. |

## Trade-offs and downsides

What you give up compared to running the stock firmware (SLZB-OS):

- **Home Assistant is effectively required.** The Native API transport is designed around the HA ESPHome integration and the `esphome-hass://` URL scheme. If you want to run the device standalone (no HA, or with a non-HA host such as Zigbee2MQTT on bare Linux talking to `socket://`), this firmware is not the right choice — stick with the stock SMLIGHT firmware.
- **Recent HA versions are required.** You need a Home Assistant version whose ESPHome integration supports `serial_proxy`, and ZHA / OTBR / Z-Wave JS versions that accept the `esphome-hass://` URL scheme.
- **No built-in web admin UI.** SLZB-OS's HTTP dashboard (device info, radio mode switching, VPN config, etc.) is gone by design. Configuration lives in YAML and is applied by re-flashing; runtime state is exposed as normal HA entities.
- **You build and flash the firmware yourself.** No pre-built binaries are published here; you compile with the ESPHome CLI or dashboard against this repo. This is the normal ESPHome workflow but is a shift from downloading a consumer-ready vendor image.
- **SLZB-OS-only features are not reproduced.** Vendor extras such as the built-in ZeroTier / WireGuard clients and the SMLIGHT cloud portal are not part of this firmware.
- **Third-party fork, not SMLIGHT-supported.** SMLIGHT ships two supported firmware paths for the SLZB adapters: their proprietary SLZB-OS and their own upstream ESPHome build ([smlight-tech/slzb-esphome](https://github.com/smlight-tech/slzb-esphome)). This project is a security-hardened fork of the latter and is not the SMLIGHT-supported build. Don't contact SMLIGHT for anything related to this custom firmware. Reflash to one of the supported firmwares first (e.g. via the [SMLIGHT web flasher](https://smlight.tech/flasher/) or USB) so the conversation is about the hardware, not this fork.

## Choose this firmware if…

- You run Home Assistant and want your SMLIGHT device (SLZB coordinator or SLWF USB-over-network gateway) to stop broadcasting a plaintext serial port on your LAN.
- You already treat the device as "one more ESPHome node" and want to configure it like the rest of your ESPHome fleet.
- You are comfortable building and flashing ESPHome firmware.

## Stick with SLZB-OS if…

- You need the vendor web UI, cloud portal, or the built-in VPN clients.
- You don't run Home Assistant, or your host software cannot use `esphome-hass://` URLs.
- You want vendor support and a signed vendor firmware image.

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
| Radio reset / bootloader entry | HA switches writing GPIO | Automatic — `serial_proxy` drives `dtr_pin` (nRESET) and `rts_pin` (bootloader) from the client's DTR/RTS modem-control signals |
| OTA | Unauthenticated | Password-protected (`ota_password`) |
| USB pass-through (`packages/usb/usb_uart.yaml`) | Plaintext TCP `:9638` | `serial_proxy` (port name `usb`) — package exists but is not `!include`d by any shipping device build in v1 |
| Radio firmware version reporting | Not exposed to HA | One-shot boot-time probe per radio (ZNP live for CC26xx in v1; Spinel / EZSP / Z-Wave stubs until v1.x); diagnostic sensors + HA template snippet vs. SMLIGHT's public catalog. See [`docs/ha-integrations/`](docs/ha-integrations/) |

For the full delta reference — everything preserved, removed, disabled-by-default, and no-longer-exposed as HA entities, with rationale — see [`docs/design.md §30 Delta from upstream`](docs/design.md).

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
   `esphome-hass://esphome/{entry_id}?port_name=zigbee` (or `thread` / `zwave`, depending on which radio the integration is talking to).

   `{entry_id}` is the ESPHome config-entry id — visible under **Settings → Devices & Services → ESPHome → (your device)**.

---

## Project structure

See [`docs/architecture.md`](docs/architecture.md) for the full layout, layer responsibilities, pin-abstraction rules, and instructions for adding a new device.

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
| **UPS I2C** | Yes | - | - | - |
| **I2C Expander** | Yes | - | - | - |
| **4G/LTE Addon** | Yes | - | - | - |
| **USB-C CC ADC** | - | Yes | Yes | - |
| **DIY Expansion** | Yes | - | - | Yes |

---

## Usage examples

See [`docs/usage.md`](docs/usage.md) for the full feature cookbook — buzzer / RTTTL melodies, WS2812 LED effects and presets, and IR transmit examples — including RMT symbol buffer overrides for boards that share the pool between IR TX and WS2812.
