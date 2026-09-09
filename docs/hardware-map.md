# Hardware map — SLZB-MR4U (rev 1.73)

**Phase 0.1 inspection artifact.**
This document is the ground-truth reference for the MR4U hardware surface as declared in the current upstream firmware.
Every pin, port, and configuration value here is copied from actual source files — nothing is inferred or assumed.
When a value is *not* present or *does not do what its name implies*, that is called out explicitly.

| Field | Value |
|---|---|
| Reflects upstream commit | `3397f6f` — *Merge pull request #4 from robelmes/fix/esphome-2026-warnings* (2026-06-13) |
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

- **✓ Full, tested**: MR4U only. Firmware built, flashed, and reliability-tested in Phases 3 and 5.
- **✓ Full, expected-to-work-by-construction**: ULTIMA, MRxU siblings, 06xU. Same shared packages, same architecture. Not personally validated. Users of these boards contribute test evidence.
- **Partial**: SLWF-09U. Encryption/password improvements apply but the raison d'être of this fork (secure Zigbee/Thread radio transport) doesn't.

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

Declared in `hw_defs/mrxu/mr4u_r1_73.yaml` under the "UART1" heading.

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

The runtime behavior today: any host on the same L2 segment as the MR4U can `nc <mr4u-ip> 7638` and get a raw bidirectional pipe to the CC26 UART with no authentication and no encryption. This is what design doc §2 identifies as the primary attack surface.

## 5. Radio 2 — EFR32 (Zigbee / Thread on Silicon Labs EFR32MG26)

Declared in `hw_defs/mrxu/mr4u_r1_73.yaml` under the "UART2" heading.

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

The user's reference MR4U hardware currently runs **SMLIGHT SLZB-OS** (the proprietary vendor firmware, not the SMLIGHT ESPHome firmware). Screenshot of the SLZB-OS serial-options web UI on that unit reveals:

| Radio | Baud | Flow control | Socket port | Radiomodule mode |
|---|:---:|:---:|:---:|---|
| CC2674P10 | 115 200 | **ON** (RTS/CTS) | 7638 | Zigbee coordinator |
| EFR32MG26 | **460 800** | **ON** (RTS/CTS) | 6638 | Matter-over-Thread |

The UI's own text states: *"Only enable [flow control] if the Zigbee/Thread firmware release notes confirm HW flow control support; otherwise leave disabled."* — which means vendor engineering has **explicitly verified** that both radio firmwares honor CTS. This retires uncertainty (a) from §6's caveats: our Phase 1 flow-control-by-default plan is safe on stock radio firmware.

#### Baud rate discrepancy

SMLIGHT SLZB-OS talks to the EFR32MG26 at **460 800** by default; the SMLIGHT ESPHome fork YAML (`uart2_baud: 115200`) initializes at **115 200**. The mismatch is bridged by a runtime `select` entity in `packages/buses/uarts/uart_baud_runtime_selector/uart2_baud_selector.yaml`:

```yaml
select:
  - id: change_baud_rate_uart2
    options: ["115200", "230400", "460800", "921600"]
    initial_option: "115200"
    restore_value: true
    # ...set_action changes UART baud at runtime
```

So the first-boot flow is:
1. ESP inits `hw_uart2` at 115 200.
2. EFR32MG26 is talking at 460 800 (radio-firmware-controlled).
3. No communication until the user opens Home Assistant and switches the select to `460800`.
4. `restore_value: true` persists the choice.

**UX consequence:** zero-config first boot doesn't work on EFR32. Phase 1 should set the `uart2_baud` substitution *and* the selector's `initial_option` to `460800` so the fork Just Works on factory MR4U hardware. CC2674P10 stays at 115 200 (matches factory).

#### What this means for the design doc

- Chip identifications tightened: **CC2674P10** and **EFR32MG26** (not generic "CC26xx" / "EFR32MG"). Update design.md §4 hardware description.
- Design doc §6's flow-control caveat ("needs verification whether radio firmware honors CTS") can be downgraded from "unknown" to "confirmed on stock SLZB-OS — verify same on our fork after Phase 1 flash".
- Design doc's implicit assumption that both radios run at 115 200 is wrong for EFR32; needs a small correction.

---

## 6. HW UART flow control — **declared but not wired**

> **Superseded by Phase 0.2.** The "Decision: enable HW flow control by default in Phase 1" below reflects Phase 0.1 thinking. Phase 0.2 subsequently found that ESPHome's `uart:` component has no `cts_pin`/`rts_pin` YAML surface and never calls `uart_set_hw_flow_ctrl()` — enabling flow control requires an upstream ESPHome change. Current plan: **flow control deferred, not shipped in Phase 1.** See design.md Phase 1 item 7 and `serial-proxy-inspection.md` §7. The rest of §6 is preserved as the Phase 0.1 audit record.

This is the most consequential finding of Phase 0.1 for the design.

The hw_def files declare CTS/RTS pin assignments for both radios. However, the packages that instantiate ESPHome's `uart:` platform do **not** consume those substitutions:

```yaml
# packages/buses/uarts/uart1_hwfc.yaml
uart:
  - id: hw_uart1
    tx_pin: ${pin_uart1_tx}
    rx_pin: ${pin_uart1_rx}
    baud_rate: ${uart1_baud}
```

Note: no `cts_pin:` or `rts_pin:` keys. The corresponding `uart1_no_hwfc.yaml` file is **byte-identical** — despite the naming distinction, there is no functional difference between the two. Same for `uart2_hwfc.yaml` / `uart2_no_hwfc.yaml`.

### Consequences and decision

1. **The MR4U firmware today does not use HW flow control.** Design doc §21.3 previously listed this as a "key unknown"; it is now a **confirmed fact**: flow control is off today.
2. **The physical MR4U board *has* the CTS/RTS pins routed to the radio flow-control inputs** (that's why the substitutions exist). Enabling flow control is a firmware-only change, no hardware modification needed.
3. **Decision: enable HW flow control by default in Phase 1** (secure firmware). Rationale:
   - Enabling ESP-side flow control is **strictly non-negative**: if the radio firmware honors CTS, we prevent RX FIFO overflow; if it doesn't, ESPHome's UART draining still runs normally, so we lose nothing.
   - Cost is 2 YAML lines per UART.
   - Aligns with correctness-by-construction: don't leave a known lever unpulled just because the failure mode hasn't been observed.

### Caveats to verify during reliability testing (Phase 3 / 5)

Flow control works end-to-end only if **both** sides participate:

- **CC26 ZNP firmware** — TI's Z-Stack ZNP images can be built with UART HW flow control (build-time option). Whether SMLIGHT's shipped ZNP image has it on is unknown; will be validated by observing whether the CC26 responds to ESP-driven RTS deassertion under sustained traffic.
- **EFR32 Spinel (RCP) firmware** — Silicon Labs' RCP has flow control as a config option. Same story: needs empirical validation.

If either radio firmware ignores CTS, ESP-side flow control still helps (drains its own FIFO faster than a passive receiver would) but does not fully prevent overflow. In that case Phase 6 becomes relevant — investigating the underlying cause rather than re-deciding whether to enable flow control.

### ESPHome-side prerequisite

Before Phase 1 can actually enable flow control, we need to confirm during Phase 0.2 (ESPHome inspection) that:

- `uart:` component's `cts_pin` / `rts_pin` keys are honored on the ESP-IDF framework (not just Arduino)
- Those keys ultimately call `uart_set_hw_flow_ctrl(UART_HW_FLOWCTRL_CTS_RTS)` at driver level

If ESPHome's ESP-IDF UART component doesn't propagate flow control (historically Arduino-first), Phase 1 has two options:
- **(a)** Upstream a fix to ESPHome. Preferred per §22.
- **(b)** Ship an external component wrapping `uart_component_esp_idf.cpp` with the flow-control call. Fallback only.

### Naming cleanup

The existing `_hwfc.yaml` / `_no_hwfc.yaml` distinction is misleading (both files are byte-identical). Phase 1 collapses this to a single package file per UART with flow control actually enabled:

- **Delete** `packages/buses/uarts/uart1_no_hwfc.yaml`, `uart2_no_hwfc.yaml`, `uart3_no_hwfc.yaml`
- **Rewrite** `packages/buses/uarts/uart1_hwfc.yaml`, `uart2_hwfc.yaml`, `uart3_hwfc.yaml` to actually include `cts_pin` and `rts_pin`
- **Alternative**: rename `_hwfc.yaml` → just `uart{n}.yaml` since the distinction no longer exists (cleaner but slightly bigger diff from upstream)

Exact naming choice deferred until we write the Phase 1 patches.

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
| Configuration package | `packages/ethernet/w5500_gpio.yaml` + `packages/ethernet/rj45_leds_gpio.yaml` | Not read as part of Phase 0.1; likely just wraps `ethernet:` platform with these pins. Worth confirming during Phase 1. |

## 8. USB and power

| Aspect | GPIO | Direction | Notes |
|---|:---:|:---:|---|
| USB device/host mode select | 47 | out (inverted) | Drives a mux that switches USB-C between "device" (ESP32-S3 native USB acts as USB device to a host PC) and "host" (routes to some other endpoint). |
| PoE→USB power passthrough | 48 | out | Enables passing PoE-derived power through to the USB-C connector. |
| USB-C CC1 sense | 4 | ADC in | Analog voltage on CC1 pin — used to detect USB-C source/sink orientation and current-advertising resistors. |
| USB-C CC2 sense | 5 | ADC in | Same for CC2. |

Full USB behavior lives in `packages/usb/` (not read for Phase 0.1 — hardware wiring only). If we later add a Phase 9 USB build variant (design doc §25 / §26), understanding how these pins interact with the ESP32-S3's native USB PHY becomes critical.

## 9. LEDs and buttons

| Element | GPIO | Inverted | Behavior on boot |
|---|:---:|:---:|---|
| Button 1 (BOOT) | 0 | yes (active-low) | Standard ESP32-S3 strapping pin. Used at power-up as the ROM bootloader entry signal (hold to enter download mode). Also exposed as an ESPHome input. |
| LED 1 | 46 | yes (active-low) | `restore_mode: RESTORE_DEFAULT_ON` — turns on at boot unless previous state overrides. |
| LED 2 | 45 | yes (active-low) | `restore_mode: ALWAYS_OFF` — off at boot regardless. |

No second button on MR4U (design doc §4 correctly matches this).

---

## 10. Current security surface (what Phase 1 must change)

This section maps the design doc's abstract "kill the plaintext ports" objective onto the *specific* config we'll modify.

### 10.1 What's currently exposed on the network

| Port | Protocol | Purpose | Auth | Encryption |
|:---:|---|---|:---:|:---:|
| **7638/tcp** | Raw TCP | CC26 UART bridge (`stream_server` on `hw_uart1`) | None | None |
| **6638/tcp** | Raw TCP | EFR32 UART bridge (`stream_server` on `hw_uart2`) | None | None |
| **6053/tcp** | ESPHome Native API | Device control + telemetry | Optional | **Currently disabled** — `api:` block in `packages/core/core.yaml` has no `encryption:` key |
| **3232/tcp** | ESPHome OTA | Firmware updates | Optional | The `esphome` OTA platform can require a password; **currently no password configured** in `packages/core/core.yaml` |
| **80/tcp** | Web server | N/A | N/A | **Not enabled** — `web_server:` block in `core.yaml` is commented out |
| **5353/udp** | mDNS | Device discovery | — | Not a threat surface per se |

### 10.2 Phase 1 concrete deltas (what our fork will change)

To satisfy design doc §7 and §23:

1. **Remove `stream_server` from both radios**
   - Delete `hal_uart1_ss_server` and `hal_uart2_ss_server` includes from `devices/mr4u_r1_73.yaml`
   - Delete `stream_server` external component from `packages/external_components/stream_server.yaml` (or delete the file and remove the include)
   - Delete `packages/stream_servers/` entirely (or at least the `ss_uart*.yaml` files we use)
   - This eliminates ports 7638 and 6638.

2. **Enable Native API encryption**
   - In `packages/core/core.yaml`, extend the `api:` block with:
     ```yaml
     api:
       encryption:
         key: !secret api_encryption_key
     ```
   - Add `api_encryption_key` to `secrets.yaml` (which is already gitignored per line 5 of `.gitignore`).
   - This closes port 6053 against unauthenticated access — connections without the correct key are rejected during Noise handshake.

3. **Password-protect OTA**
   - Extend the `ota:` block with `password: !secret ota_password`.
   - Add `ota_password` to `secrets.yaml`.

4. **Add `serial_proxy` in place of `stream_server`**
   - Two instances, one per UART, subscribed via the Native API.
   - Package names TBD — likely `packages/buses/uarts/uart_ctrl/serial_proxy1.yaml` and `serial_proxy2.yaml` (following the existing pattern), or new folder `packages/serial_proxies/`.

5. **Enable HW UART flow control** *(deferred — see §6 supersession note)*
   - Rewrite `packages/buses/uarts/uart1_hwfc.yaml` and `uart2_hwfc.yaml` to include `cts_pin: ${pin_uart1_cts}` and `rts_pin: ${pin_uart1_rts}` (and analogous for UART2).
   - Delete the byte-identical `_no_hwfc.yaml` siblings (they no longer add value).
   - See §6 for the full rationale and caveats.

Web server does NOT need removal — upstream already leaves it commented out. Design doc §7 language about "remove `web_server`" can be softened to "keep `web_server` disabled (already the case)."

---

## 11. Cross-references to the design document

Findings from this inspection that should be reflected back into `docs/design.md`:

| Finding | Affects design doc section | Nature of change |
|---|---|---|
| MR4U runs on ESP32-S3 with PSRAM, 16 MB flash | §4 Hardware architecture | Confirmed — no change needed |
| Radios are CC26 (Zigbee/Thread) and EFR32 (Zigbee/Thread) | §4, §5, §9, §11 | Confirmed — matches design assumptions |
| HW flow control declared but not wired in ESPHome UART bus | §21.3 (key unknown) | Downgrade §21.3 from "unknown" to "confirmed disabled today; needs enabling only if reliability testing demands it" |
| `web_server` already disabled upstream | §7 target-state description | Soften wording — no removal work needed, just keep it disabled |
| `stream_server` uses `oxan/esphome-stream-server` external component (not built-in) | §7 firmware structure | Add note: our fork removes an external component dependency, not a built-in ESPHome feature. Slightly reduces future maintenance risk (no need to track upstream ESPHome changes for that component). |
| UART2 TX/RX are swapped between `mr4u_r1_73.yaml` and generic `r1_73.yaml` | §4 Hardware architecture — not currently mentioned | Add a brief note so future readers don't get confused editing the wrong file |
| Native API is present but unencrypted; OTA has no password | §2 problem statement | Confirmed — the "unencrypted by default" premise is real, verified in `core.yaml` |

These are captured as *observations*; whether to edit the design document is the user's call. Recommendation: batch these small corrections into a single design-doc revision after all Phase 0 targets complete, so the doc is updated once against fully-validated ground truth.

---

## 12. What Phase 0.1 did NOT inspect

To keep this document scoped and honest:

- `packages/ethernet/*.yaml` — pin usage extracted but the actual `ethernet:` platform config not read
- `packages/usb/*.yaml` — USB mode-switching logic not analyzed
- `packages/diagnostics/diagnostics.yaml` — diagnostic sensors config not read
- `packages/bluetooth/bluetooth.yaml` — whether Bluetooth proxy is enabled (relevant to design doc §21.8)
- `packages/buses/uarts/uart_baud_runtime_selector/*.yaml` — runtime baud-rate switching mechanism, likely relevant to Phase 7 (radio maintenance actions)
- Whether ESPHome's `uart:` component actually propagates `cts_pin`/`rts_pin` to the ESP-IDF UART driver's HW flow control — needs Phase 0.2 (ESPHome source inspection)
- Whether `serial_proxy` still exists in current ESPHome and what its config schema looks like — this is Phase 0.2

These are deferred to their appropriate Phase 0 targets or later phases.
