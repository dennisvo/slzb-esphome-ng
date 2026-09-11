# Hardware map — SLZB-MR4U (rev 1.73)

Current-state reference for the MR4U hardware surface: pins, radios, and network exposure, together with how this fork's package tree wires them.
Every value here is copied directly from `hw_defs/**` and `packages/**` in this repo.
Where upstream (`smlight-tech/slzb-esphome`) does something different, the difference is called out inline.

| Field | Value |
|---|---|
| Upstream reference commit | `3397f6f` — *Merge pull request #4 from robelmes/fix/esphome-2026-warnings* (2026-06-13) — the commit this fork was branched from |
| Upstream source | https://github.com/smlight-tech/slzb-esphome |
| Board | SLZB-MR4U rev 1.73 |
| Root YAML | `mr4u-r1-73.yaml` → `devices/mr4u_r1_73.yaml` |
| Hardware definitions | `hw_defs/mrxu/mr4u_r1_73.yaml` |

---

## 0. Scope — which SMLIGHT devices this design targets

Although this hardware map documents the MR4U in detail (because it is our reference test hardware), the slzb-esphome-ng variant is **not MR4U-specific**. Upstream's layered architecture (shared `packages/` consumed by every `devices/*.yaml` composition) means our security changes automatically apply to every device that uses those packages.

| Device | Radios | Applicability of this fork |
|---|:---:|---|
| **ULTIMA** | 3 (CC26 + EFR32 + ZW-800/Z-Wave) | ✓ Full — Native API encryption + OTA password + 3 `serial_proxy` instances replacing 3 plaintext bridges. Biggest security uplift. |
| **MRxU** family (MR4U, MR2U, etc.) | 2 (CC26 + EFR32) | ✓ Full — this is our **primary target and only tested platform** |
| **06xU** | 1 (CC26 *or* EFR32) | ✓ Full — same shared-package changes; 1 `serial_proxy` instance |
| **SLWF-09U** | 0 (Wi-Fi/BT dongle only) | Partial — Native API encryption + OTA password apply. No radios, so no plaintext-UART attack surface to remove. Marginal benefit. |

**Support levels defined:**

- **✓ Full, tested**: MR4U only. Reference test hardware — firmware built, flashed, and validated here first.
- **✓ Full, expected-to-work-by-construction**: ULTIMA, MRxU siblings, 06xU. Same shared packages, same architecture. Not personally validated on physical hardware; user test reports welcome.
- **Partial**: SLWF-09U. Native API encryption and password-protected OTA apply, but the fork's core purpose (secure Zigbee/Thread radio transport) doesn't — there are no radios to protect.

The rest of this document uses MR4U as the concrete example throughout. Where a pin or port is specific to MR4U (as opposed to shared MRxU family or generic SMLIGHT convention), that is called out.

---

## 1. Board identity

| Property | Value | Source |
|---|---|---|
| `device_name` | `mrxu-r1-73` | `hw_defs/mrxu/mr4u_r1_73.yaml` |
| `friendly_name` | `MRXU >` | `hw_defs/mrxu/mr4u_r1_73.yaml` |
| `esphome.project.name` | `SMLIGHT.mrxu-r1-73` | `packages/core/core.yaml` |
| `esphome.project.version` | `1.00` | `packages/core/core.yaml` |
| `name_add_mac_suffix` | `true` (device name gets MAC suffix at runtime) | `packages/core/core.yaml` |

## 2. SoC and platform

| Property | Value |
|---|---|
| MCU | ESP32-S3 (variant with PSRAM — S3R2) |
| ESPHome board target | `esp32-s3-devkitc-1` |
| Framework | ESP-IDF (`framework.type: esp-idf`) |
| Flash size | 16 MB (declared as substitution `${esp32_flash_size}: 16Mb`, currently commented out in `esp32:` block) |
| PSRAM mode | quad |
| PSRAM speed | 80 MHz |
| CPU frequency | 240 MHz (substitution declared, currently commented out in `esp32:` block) |
| LWIP max sockets | 16 (`CONFIG_LWIP_MAX_SOCKETS`) |

> **Note:** `flash_size` and `cpu_frequency` substitutions are declared in `hw_defs` but the corresponding lines in `packages/core/core.yaml` are commented out. ESPHome falls back to defaults (auto-detected for flash, 240 MHz default for CPU on ESP32-S3). Effect on firmware behavior: none observable in practice, but worth knowing if we ever set `flash_size` explicitly.

---

## 3. Complete pin map (all declared GPIOs)

Sorted by GPIO number. All values pulled from `hw_defs/mrxu/mr4u_r1_73.yaml`.

| GPIO | Function | Direction | Inverted | Notes |
|:---:|---|:---:|:---:|---|
| 0 | Button 1 | in | yes | ESP32-S3 strapping pin (BOOT). Active-low button. |
| 1 | RJ45 LEDs | out | yes | Drives the Ethernet jack activity LED(s). |
| 2 | W5500 chip select (Ethernet) | out | — | SPI CS for the Wiznet W5500 Ethernet controller. |
| 4 | USB-C CC1 ADC | in | — | For USB power/mode detection via CC pin voltage. |
| 5 | USB-C CC2 ADC | in | — | Second CC pin. |
| 8 | **UART2 TX** (EFR32) | out | — | **See §5.2 — differs between MR4U-specific and generic hw_def.** |
| 9 | UART2 flash-enter control (EFR32) | out | yes | GPIO output to a switch entity; drives EFR32 bootloader enter line. |
| 10 | **UART2 RX** (EFR32) | in | — | **See §5.2 — differs between MR4U-specific and generic hw_def.** |
| 11 | UART2 CTS (EFR32) | in | — | **Declared but not wired to ESPHome UART bus — see §6.** |
| 12 | UART2 RTS (EFR32) | out | — | **Declared but not wired to ESPHome UART bus — see §6.** |
| 13 | UART2 reset control (EFR32) | out | yes | Drives EFR32 reset line. |
| 14 | UART1 RTS (CC26) | out | — | **Declared but not wired to ESPHome UART bus — see §6.** |
| 15 | UART1 CTS (CC26) | in | — | **Declared but not wired to ESPHome UART bus — see §6.** |
| 16 | UART1 flash-enter control (CC26) | out | yes | Drives CC26 bootloader enter line. |
| 17 | UART1 TX (CC26) | out | — | |
| 18 | UART1 RX (CC26) | in | — | |
| 21 | UART1 reset control (CC26) | out | yes | Drives CC26 reset line. |
| 38 | W5500 interrupt | in | — | Ethernet controller IRQ. |
| 39 | SPI MOSI | out | — | Shared SPI bus (currently only W5500 attached). |
| 40 | W5500 reset | out | — | Ethernet controller reset. |
| 41 | SPI MISO | in | — | Shared SPI bus. |
| 42 | SPI SCLK | out | — | Shared SPI bus. |
| 43 | I²C SCL | out (open-drain) | — | I²C bus (`hal_i2c` package included but no I²C devices attached on MR4U). |
| 44 | I²C SDA | i/o | — | Ditto. |
| 45 | LED 2 | out | yes | Second status LED. `restore_mode: ALWAYS_OFF`. |
| 46 | LED 1 | out | yes | Primary status LED. `restore_mode: RESTORE_DEFAULT_ON`. |
| 47 | USB mode select (USB device/host) | out | yes | Drives a USB routing mux. |
| 48 | PoE-to-USB power switch | out | — | Enables passing PoE power to the USB-C connector. |

**Pins not used by MR4U (available if repurposed):** all remaining ESP32-S3 GPIOs (3, 6, 7, 19, 20, 26 – 37 depending on the exact package variant — the R2 variant loses some pins to PSRAM). Do **not** touch 26–32 without confirming the specific ESP32-S3 chip variant used on the MR4U board; those are typically reserved for PSRAM on -R2 parts.

---

## 4. Radio 1 — CC26 (Zigbee / Thread on TI CC2674P10)

Declared in `hw_defs/mrxu/mr4u_r1_73.yaml` under the "UART1" heading. The table below records the physical wiring and the upstream `stream_server` bridge on port 7638. This fork replaces that bridge with a `serial_proxy` instance carried over the encrypted Native API; port 7638 is not exposed. See [design.md](design.md) for the transport model.

| Aspect | Value | Source |
|---|---|---|
| Radio family | Texas Instruments **CC2674P10** (integrated PA variant) | Substitution `uart1_radio_name: CC26`; specific part confirmed via SMLIGHT SLZB-OS web UI on the reference hardware (see §5.3). |
| Data TX (ESP → radio) | GPIO17 | `pin_uart1_tx` |
| Data RX (radio → ESP) | GPIO18 | `pin_uart1_rx` |
| CTS (declared) | GPIO15 | `pin_uart1_cts` — **not consumed by ESPHome UART bus, see §6** |
| RTS (declared) | GPIO14 | `pin_uart1_rts` — **not consumed by ESPHome UART bus, see §6** |
| Baud rate | 115 200 | `uart1_baud` |
| Reset line | GPIO21, active-low | `pin_uart1_rst` + `uart1_rst_inverted: true`. Exposed as a switch entity `uart1_rst` (`packages/buses/uarts/uart_ctrl/uart1_rst_common.yaml`). |
| Flash/bootloader-enter line | GPIO16, active-low | `pin_uart1_flash` + `uart1_flash_inverted: true`. Exposed as a switch entity. |
| **Current TCP exposure** | Port **7638**, plaintext | `packages/stream_servers/ss_uart1.yaml` + `uart1_default_port: 7638` |
| Stream server implementation | `oxan/esphome-stream-server` external component | `packages/external_components/stream_server.yaml` |

The runtime behavior of upstream on port 7638: any host on the same L2 segment as the MR4U can `nc <mr4u-ip> 7638` and get a raw bidirectional pipe to the CC26 UART with no authentication and no encryption. This is the primary attack surface this fork removes; see [design.md](design.md) §2.

## 5. Radio 2 — EFR32 (Zigbee / Thread on Silicon Labs EFR32MG26)

Declared in `hw_defs/mrxu/mr4u_r1_73.yaml` under the "UART2" heading. Same pattern as §4: upstream exposes port 6638 as a plaintext bridge; this fork replaces it with a `serial_proxy` instance over the encrypted Native API. Port 6638 is not exposed.

### 5.1 Nominal pin mapping

| Aspect | Value | Source |
|---|---|---|
| Radio family | Silicon Labs **EFR32MG26** | Substitution `uart2_radio_name: EFR32`; specific part confirmed via SMLIGHT SLZB-OS web UI on the reference hardware (see §5.3). |
| Data TX (ESP → radio) | **GPIO8** | `pin_uart2_tx` in `mr4u_r1_73.yaml` |
| Data RX (radio → ESP) | **GPIO10** | `pin_uart2_rx` in `mr4u_r1_73.yaml` |
| CTS (declared) | GPIO11 | `pin_uart2_cts` — **not consumed by ESPHome UART bus, see §6** |
| RTS (declared) | GPIO12 | `pin_uart2_rts` — **not consumed by ESPHome UART bus, see §6** |
| Baud rate | 115 200 | `uart2_baud` |
| Reset line | GPIO13, active-low | `pin_uart2_rst` + `uart2_rst_inverted: true` |
| Flash/bootloader-enter line | GPIO9, active-low | `pin_uart2_flash` + `uart2_flash_inverted: true` |
| **Current TCP exposure** | Port **6638**, plaintext | `packages/stream_servers/ss_uart2.yaml` + `uart2_default_port: 6638` |

### 5.2 Discrepancy — `mr4u_r1_73.yaml` vs `r1_73.yaml`

Upstream ships **two** hw-def files for the MRxU family:

| File | UART2 TX | UART2 RX |
|---|---|---|
| `hw_defs/mrxu/mr4u_r1_73.yaml` (MR4U-specific) | **GPIO8** | **GPIO10** |
| `hw_defs/mrxu/r1_73.yaml` (family-generic)     | **GPIO10** | **GPIO8** |

The MR4U device composition (`devices/mr4u_r1_73.yaml`) explicitly includes the **MR4U-specific** file, so the effective values on our target board are TX=GPIO8, RX=GPIO10.

The generic `r1_73.yaml` file appears to be unused by any composition file currently in the repo (`grep` finds no `!include` referencing it besides itself). It may be either (a) a template for future MRxU variants with a different silicon layout, (b) an artifact left over from a refactor, or (c) valid for a different revision that isn't yet declared as a device.

**Implication for our firmware:** none — we inherit MR4U-specific values via the correct include chain. This is documented here purely so a future maintainer editing `r1_73.yaml` doesn't wrongly assume it's what MR4U uses.

### 5.3 Vendor firmware observations (SLZB-OS on the reference MR4U)

The reference MR4U hardware initially shipped with **SMLIGHT SLZB-OS** (the proprietary vendor firmware, not SMLIGHT's ESPHome firmware). The SLZB-OS serial-options web UI on that unit shows:

| Radio | Baud | Flow control | Socket port | Radiomodule mode |
|---|:---:|:---:|:---:|---|
| CC2674P10 | 115 200 | **ON** (RTS/CTS) | 7638 | Zigbee coordinator |
| EFR32MG26 | **460 800** | **ON** (RTS/CTS) | 6638 | Matter-over-Thread |

Two facts to carry forward:

**Radio firmware honors CTS.** The UI's own text states: *"Only enable [flow control] if the Zigbee/Thread firmware release notes confirm HW flow control support; otherwise leave disabled."* Both radios ship with flow control on, so both firmwares honor CTS. In this fork, HW flow control is an opt-in axis via the `uart*_hw_flow: true|false` substitution (see §6); MR4U ships with it `false` to match SMLIGHT's ESPHome firmware default.

**EFR32 baud is 460 800, not 115 200.** SLZB-OS talks to the EFR32MG26 at 460 800; SMLIGHT's own ESPHome YAML sets `uart2_baud: 115200` and relies on a runtime `select` entity (`packages/buses/uarts/uart_baud_runtime_selector/uart2_baud_selector.yaml`) to switch — first boot doesn't work until the user changes the option in Home Assistant. This fork sets `uart2_baud: 460800` in `hw_defs/mrxu/mr4u_r1_73.yaml` and pins the selector's `initial_option` to the substitution, so first boot works without intervention. CC2674P10 stays at 115 200 (matches factory).

---

## 6. HW UART flow control — shipped via fork-local custom component

The hw_def files declare CTS/RTS pin assignments for both radios (see §4, §5). Upstream ESPHome's `uart:` platform has no YAML surface for classic RTS/CTS handshake (`flow_control_pin` is for RS485 driver-enable only) and never calls ESP-IDF's `uart_set_hw_flow_ctrl()`. This fork closes that gap with a custom external component.

The `uart{1,2,3}_hw_flow.yaml` packages declare both blocks:

```yaml
# packages/buses/uarts/uart1_hw_flow.yaml (excerpt)
uart:
  - id: hw_uart1
    tx_pin: ${pin_uart1_tx}
    rx_pin: ${pin_uart1_rx}
    baud_rate: ${uart1_baud}

uart_hw_flow:
  - uart_id: hw_uart1
    cts_pin: ${pin_uart1_cts}
    rts_pin: ${pin_uart1_rts}
    enabled: ${uart1_hw_flow}
```

The `uart{1,2,3}_no_hwfc.yaml` siblings are byte-identical to the `_hw_flow` versions except for the missing `uart_hw_flow:` block; no shipping device uses the `_no_hwfc` variant.

Behavior:

1. **Custom component runs after the UART driver is up.** `components/uart_hw_flow/` (setup priority 250, same slot as `radio_probe`) looks up the ESP-IDF port number for the referenced `uart:` id and calls `uart_set_pin()` + `uart_set_hw_flow_ctrl(port, UART_HW_FLOWCTRL_CTS_RTS, 122)` when `enabled` is true. `enabled: false` makes it a no-op — the config surface stays stable regardless of the substitution value.
2. **The upstream limitation is in ESPHome, not on the MR4U.** The physical board *has* CTS/RTS routed to the radio flow-control inputs, so enabling flow control is a firmware-only change with no hardware modification needed.

### Radio firmware compatibility

SLZB-OS ships both radios with flow control on by default (see §5.3), so both the CC2674P10 ZNP image and the EFR32MG26 Spinel image honor CTS. Enabling ESP-side flow control is safe on both stock radio firmwares.

### Status in this fork

The component ships and is wired into every device with a radio UART. MR4U defaults keep flow control `false` on both UARTs — matching SMLIGHT's ESPHome firmware baseline — with the axis available for opt-in per UART via the `uart*_hw_flow: true|false` substitution in `hw_defs/**`. Chiefly relevant for future EFR32MG24 Thread builds, whose catalog entries advertise `hwFlow: true`; the Home Assistant Jinja filter chain refuses to offer a mismatched firmware image.

---

## 7. Ethernet (W5500 SPI)

| Aspect | Value | Notes |
|---|---|---|
| Controller | Wiznet W5500 | Standard 10/100 SPI Ethernet controller. |
| SPI bus | Shared (see §3) | MOSI=39, MISO=41, SCLK=42 |
| CS | GPIO2 | |
| Interrupt | GPIO38 | |
| Reset | GPIO40 | |
| RJ45 LED(s) | GPIO1, active-low | Physical activity LEDs on the jack. |
| Configuration package | `packages/ethernet/w5500_gpio.yaml` + `packages/ethernet/rj45_leds_gpio.yaml` | Wraps the ESPHome `ethernet:` platform with these pins. |

## 8. USB and power

| Aspect | GPIO | Direction | Notes |
|---|:---:|:---:|---|
| USB device/host mode select | 47 | out (inverted) | Drives a mux that switches USB-C between "device" (ESP32-S3 native USB acts as USB device to a host PC) and "host" (routes to some other endpoint). |
| PoE→USB power passthrough | 48 | out | Enables passing PoE-derived power through to the USB-C connector. |
| USB-C CC1 sense | 4 | ADC in | Analog voltage on CC1 pin — used to detect USB-C source/sink orientation and current-advertising resistors. |
| USB-C CC2 sense | 5 | ADC in | Same for CC2. |

Full USB behavior lives in `packages/usb/`. See [design.md](design.md) for the post-v1 USB build variant.

## 9. LEDs and buttons

| Element | GPIO | Inverted | Behavior on boot |
|---|:---:|:---:|---|
| Button 1 (BOOT) | 0 | yes (active-low) | Standard ESP32-S3 strapping pin. Used at power-up as the ROM bootloader entry signal (hold to enter download mode). Also exposed as an ESPHome input. |
| LED 1 | 46 | yes (active-low) | `restore_mode: RESTORE_DEFAULT_ON` — turns on at boot unless previous state overrides. |
| LED 2 | 45 | yes (active-low) | `restore_mode: ALWAYS_OFF` — off at boot regardless. |

No second button on MR4U (design doc §4 correctly matches this).

---

## 10. Network exposure and what this fork changes

Compare the plaintext-ports baseline in upstream `smlight-tech/slzb-esphome` with what this fork ships.

### 10.1 Upstream exposure (baseline)

| Port | Protocol | Purpose | Auth | Encryption |
|:---:|---|---|:---:|:---:|
| **7638/tcp** | Raw TCP | CC26 UART bridge (`stream_server` on `hw_uart1`) | None | None |
| **6638/tcp** | Raw TCP | EFR32 UART bridge (`stream_server` on `hw_uart2`) | None | None |
| **6053/tcp** | ESPHome Native API | Device control + telemetry | Optional | Disabled — `api:` block in `packages/core/core.yaml` has no `encryption:` key |
| **3232/tcp** | ESPHome OTA | Firmware updates | Optional | No password configured |
| **80/tcp** | Web server | N/A | N/A | Not enabled — `web_server:` block commented out |
| **5353/udp** | mDNS | Device discovery | — | Not a threat surface per se |

### 10.2 This fork's changes

1. **`stream_server` removed from both radios.** Deleted `stream_server` external component; ports 7638 and 6638 are no longer opened.
2. **Native API encryption on.** `api.encryption.key: !secret api_encryption_key` in `packages/core/core.yaml`. Connections without the correct PSK are rejected during Noise handshake.
3. **OTA password-protected.** `password: !secret ota_password` on the `ota:` block.
4. **`serial_proxy` in place of `stream_server`.** One instance per radio UART, carried over the encrypted Native API. See `packages/serial_proxies/`.
5. **HW UART flow control — planned, not yet shipped.** See §6.
6. **Web server stays off.** Upstream already leaves it disabled; this fork keeps it that way.

---

## 11. Related docs

- [design.md](design.md) — architecture and phase model, including the transport and flow-control designs summarized above.
- [architecture.md](architecture.md) — repo layout and package model.
- [roadmap.md](roadmap.md) — what's shipped, planned, and deferred (including `components/uart_hw_flow/`).
