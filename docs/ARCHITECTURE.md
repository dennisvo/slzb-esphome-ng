# Architecture

Short public-facing overview of how this fork is organized. For deep rationale, threat modeling, and roadmap see `design.md`.

---

## 1. What this fork is

A **security-hardened** ESPHome firmware for SMLIGHT-style network Zigbee/Thread/Z-Wave gateways (SLZB-MR4U and siblings).

Upstream (`smlight-tech/slzb-esphome`) exposes every radio UART as a plaintext TCP port. This fork replaces that with an **encrypted ESPHome Native API** carrying [`serial_proxy`](https://esphome.io/components/serial_proxy.html) streams to Home Assistant.

Result: on a running device, only **one** TCP port is open — `6053/tcp` (encrypted Native API). No `6638`, `7638`, `8638`, `9638`, `80`.

---

## 2. Repository layout

```
.
├── mr4u-r1-73.yaml        ← ESPHome build targets (top-level entry points,
├── mrxu-r1-73.yaml          each is a one-line `!include devices/…`)
├── 06xu-r1-73.yaml          Use these when the whole repo is cloned into
├── ultima-r1-04.yaml        your ESPHome config directory.
├── slw09u-r1-01.yaml
│
├── importable/            ← Thin (~15-line) files that pull the device
│   └── slzb-*.yaml          composition from GitHub via ESPHome's remote
│                            `packages:` feature. Copy-paste one of these
│                            into the ESPHome dashboard's New-Device flow.
│
├── devices/               ← Device composition: which packages a given
│   └── *.yaml               board wires together. NO pin numbers here.
│
├── hw_defs/               ← Per-board hardware definition. Substitutions
│   └── <family>/<rev>.yaml  for pins, inversion flags, radio names, buses.
│                            THE SINGLE SOURCE OF TRUTH for hardware.
│
├── packages/              ← Reusable YAML modules composed by devices/.
│   ├── core/                Core ESPHome config (API encryption, OTA,
│   │                        logger). Shared by every device.
│   ├── diagnostics/         Uptime, WiFi/Ethernet info, restart button.
│   ├── bluetooth/           BLE proxy (disabled by default).
│   ├── wifi/                Optional WiFi transport.
│   ├── buses/               UART / I2C / SPI bus declarations.
│   ├── ioexp/               TCA9555 IO expander (Ultima).
│   ├── ethernet/            W5500 driver variants (GPIO / TCA9555 int/rst).
│   ├── serial_proxies/      ★ Secure radio-UART bridges (Phase 1 addition).
│   ├── input/, leds/        HA-facing button/LED entities.
│   ├── addons/              4G, UPS, Z-Wave addon presence sensors.
│   └── power/, usb/         PoE, USB-C CC ADC, USB host pass-through.
│
├── components/            ← Custom ESPHome components local to the repo.
├── libraries/             ← Static asset libraries (IR codes, RTTTL, etc.).
├── docs/                  ← This directory. Design, inspections, hardware map.
├── secrets.example.yaml   ← Template — copy to secrets.yaml before build.
└── .github/workflows/     ← Continuous integration (see §6).
```

Direction of dependency (bottom depends on top):

```
mr4u-r1-73.yaml
    └─ devices/mr4u_r1_73.yaml
         ├─ hw_defs/mrxu/mr4u_r1_73.yaml        ← pins + flags
         ├─ packages/core/core.yaml              ← encrypted API, OTA
         ├─ packages/buses/uarts/uart1_hwfc.yaml
         ├─ packages/buses/uarts/uart2_hwfc.yaml
         ├─ packages/serial_proxies/sp_uart1.yaml  ← Zigbee, encrypted
         ├─ packages/serial_proxies/sp_uart2.yaml  ← Thread, encrypted
         └─ … (LEDs, buttons, ethernet, diagnostics)
```

---

## 3. Layer responsibilities

| Layer | Owns | Never touches |
|---|---|---|
| `hw_defs/` | Pin numbers, inversion flags, board revision constants, radio friendly names | Component logic, HA entities |
| `packages/` (except `core/`) | Component definitions parameterized by substitutions | Absolute pin numbers |
| `packages/core/` | ESPHome runtime (api, ota, logger) | Anything hardware-specific |
| `devices/` | *Which* packages a board includes | Pin numbers, component internals |
| Root `*.yaml` | Build target entry point (one-line include) | Anything else |

Rule: **a pin number appears exactly once, in `hw_defs/`.** All downstream consumers reach it via `${pin_*}` substitutions.

---

## 4. Pin abstraction pattern

Two patterns coexist because the fleet mixes bare-GPIO boards (MR4U, MRxU, 06xU) with IO-expander boards (Ultima):

- **Direct substitution** when the pin schema is fixed:
  `pin: ${pin_buzzer}` — always ESP32 GPIO, no schema variation.
- **Package variant** when the same *function* can be routed through different controllers:
  `packages/leds/led1_gpio.yaml` vs `packages/leds/led1_tca9555.yaml`. The device composition picks one.

`serial_proxy` also comes in two variants:
`packages/serial_proxies/sp_uart{N}.yaml` (GPIO `dtr_pin`/`rts_pin`) and `packages/serial_proxies/sp_uart{N}_tca9555.yaml` (TCA9555-attached).

Do **not** try to interpolate substitutions inside `!include` paths — ESPHome does not expand them there.

---

## 5. Security architecture (Phase 1)

| Concern | Mechanism |
|---|---|
| Radio UART transport | `serial_proxy` component (upstream ESPHome) tunneling over Native API |
| Encryption | Noise `NNpsk0` + ChaCha20-Poly1305, PSK from `!secret api_encryption_key` |
| OTA | Password-protected via `!secret ota_password` |
| Radio reset / bootloader entry | `serial_proxy.dtr_pin` (nRESET) + `serial_proxy.rts_pin` (bootloader) — asserted automatically by the client (`zigpy-znp`, `bellows`, `universal-silabs-flasher`, `zwave-js`) via modem-control signals over the API |
| Management UI | `web_server` disabled (kept off upstream too) |
| Bluetooth proxy | Disabled by default |

HA-side connection URL per radio:
`esphome-hass://esphome/{entry_id}?port_name=<zigbee|thread|zwave|usb>`

Threats **out of scope**: physical access to the device, compromised HA host, malicious radio firmware. Encryption is *in addition to* normal network segmentation, not a replacement.

Full threat model, alternatives considered, and phase-by-phase acceptance criteria live in `design.md` §1–§20.

---

## 6. Continuous Integration

`.github/workflows/esphome-config-check.yml` runs on every push and PR: it invokes `esphome config` on each of the five build targets (using a stub `secrets.yaml`) so any YAML syntax error, missing include, unknown substitution, or schema violation surfaces before it ever reaches the flasher.

CI **validates**; it does not compile a full firmware binary (that would need matching PlatformIO toolchains and burns CI minutes with little added benefit). Full compile is your local `esphome compile` step before flashing.

---

## 7. Adding a new device

1. Add a hardware definition under `hw_defs/<family>/<revision>.yaml` (pin substitutions only).
2. Add a device composition under `devices/<name>_<revision>.yaml` mirroring an existing one.
3. Add a one-line root `*.yaml` at the repo root: `!include devices/<name>_<revision>.yaml`.
4. Add the new root file to `.github/workflows/esphome-config-check.yml` so CI validates it.
5. `esphome config <name>-<revision>.yaml` locally.

No changes to any `packages/*` should be required.

---

## 8. Where to read next

- `docs/design.md` — full design rationale, threat model, phased roadmap.
- `docs/hardware-map.md` — MR4U pin map (UARTs, resets, boot pins, LEDs) plus per-board variants.
- `docs/serial-proxy-inspection.md` — deep dive on the upstream `serial_proxy` component.
- `docs/serialx-inspection.md` — the HA-side `serialx` package and `esphome-hass://` scheme.
- `docs/zha-zigpy-inspection.md` — the ZHA → zigpy → radio-backend chain.
- `docs/otbr-inspection.md` — the HA OTBR add-on architecture.
