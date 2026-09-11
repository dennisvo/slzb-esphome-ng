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

Install the **ESPHome Device Builder** add-on in Home Assistant, create a new device, copy-paste the YAML file for your board, adjust to your liking, make sure `secrets.yaml` contains the `api_encryption_key` and `ota_password` entries the file references, and hit **Install**.

The walkthrough below spells that out; the ESPHome CLI works the same way with the equivalent commands.

#### 1. Create the device in the ESPHome dashboard

1. Open **Settings → Add-ons → ESPHome Device Builder** in Home Assistant.
2. Click **+ New device** → give it a name (e.g. `slzb-mr4u`) → pick **ESP32-S3** when prompted → click through to the end and **skip the "Install" step**.
3. On the new device tile, click **Edit** to open the YAML editor.
4. **Delete everything the dashboard generated** and paste the contents of the file that matches your board from [`importable/`](importable/):

   | Board | Copy from |
   |---|---|
   | SLZB-MR4U r1.73 | [`importable/slzb-mr4u.yaml`](importable/slzb-mr4u.yaml) |
   | SLZB-MRxU r1.73 | [`importable/slzb-mrxu.yaml`](importable/slzb-mrxu.yaml) |
   | SLZB-06 / 07 (`06xU`) r1.73 | [`importable/slzb-06xu.yaml`](importable/slzb-06xu.yaml) |
   | Ultima r1.04 | [`importable/slzb-ultima.yaml`](importable/slzb-ultima.yaml) |
   | SLWF-09U r1.01 | [`importable/slwf-09u.yaml`](importable/slwf-09u.yaml) |

5. Optional tweaks: uncomment `device_name` / `friendly_name` in the `substitutions:` block to override the hostname / HA display name, or change `ref: main` to a release tag / commit SHA to pin a version. Save.

#### 2. Fill in `secrets.yaml`

The pasted file references two secrets. Open `secrets.yaml` in the dashboard's **Secrets Editor** (top-right menu) and make sure it contains at least:

```yaml
api_encryption_key: "<base64 32-byte key>"    # openssl rand -base64 32
ota_password: "<any strong secret>"
# wifi_ssid: "..."                            # only if you're not Ethernet-only
# wifi_password: "..."
```

A template is provided as [`secrets.example.yaml`](secrets.example.yaml). Keep the `api_encryption_key` handy — you'll paste the same value into Home Assistant in step 4.

#### 3. Install the firmware

- **First install — over USB.** Plug the device into the machine running the ESPHome dashboard, click **Install → Plug into the computer running ESPHome Dashboard**, and pick the serial port. One-time step; there is no OTA path from stock SLZB-OS to this firmware.
- **Subsequent updates — over the network (OTA).** Once this firmware is running, use **Install → Wirelessly**. The dashboard authenticates with the `ota_password` from `secrets.yaml`.

#### 4. Adopt the device in Home Assistant

Home Assistant normally discovers the ESPHome node automatically (**Settings → Devices & Services → Discovered**). If it doesn't, add it via **+ Add Integration → ESPHome** and enter the device's IP or hostname. When prompted for the encryption key, paste the same `api_encryption_key` value you put in `secrets.yaml`.

The device now shows up as an ESPHome device with diagnostic sensors, LED / button entities, and one virtual serial port per radio.

#### 5. Point ZHA / OTBR / Z-Wave JS at the radios

Each radio is reached over the encrypted Native API using an `esphome-hass://` URL rather than the usual `socket://ip:port`. Grab the `{entry_id}` from **Settings → Devices & Services → ESPHome → (your device)** (it's in the URL of the config-entry page), then:

- **ZHA (Zigbee)** — **+ Add Integration → Zigbee Home Automation → Manual radio type**, radio type `znp` (CC26xx) or `ezsp` (EFR32 Zigbee), serial port:
  `esphome-hass://esphome/{entry_id}?port_name=zigbee`
- **OpenThread Border Router (Thread)** — port name `thread`.
- **Z-Wave JS** — port name `zwave`.

Rebuild your Zigbee network as you would with any coordinator swap.

#### Customizing or hacking on the firmware

If you want to modify the firmware (add sensors, tweak logic, contribute back), clone the full repo into your ESPHome working directory and build from the root `*.yaml` targets (`mr4u-r1-73.yaml`, `mrxu-r1-73.yaml`, `06xu-r1-73.yaml`, `ultima-r1-04.yaml`, `slw09u-r1-01.yaml`). See [`docs/architecture.md`](docs/architecture.md) for the layer breakdown and the rules for adding a new device.

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
