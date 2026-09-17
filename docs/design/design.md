# Secure Encrypted Zigbee/Thread Serial Gateway on ESPHome

## 1. Purpose

**Motivation.** The stock SMLIGHT firmware for this class of hardware (SLZB-OS on the MR4U) has a security posture that does not meet the bar we want for a device sitting on the LAN and terminating an entire home's Zigbee and Thread networks:

- The Zigbee and Thread UARTs are exposed as **raw, unencrypted TCP sockets** (`:6638` / `:7638`, and `:8638` for USB passthrough). Any host on the LAN that can reach those ports can read, inject, disrupt, or take over the serial stream.
- The **only** access control offered on those streams is a source-IP allow-list, which is an operator opt-in and off by default. On a default install, anything on the LAN can connect to your Zigbee/Thread coordinator.
- The management web UI runs on **plain HTTP** (`:80`, no TLS). The admin password — which gates changing radio modes, flashing IEEE addresses, entering the radio bootloader, configuring VPN, and every other privileged action — crosses the LAN in cleartext on every login. So do any session cookies or tokens the UI sets.
- There is no application-level authentication or encryption of the coordinator ↔ Home Assistant path. Firewalling can hide ports but cannot make the underlying protocol trustworthy.

The goal of this project is to **replace the in-device firmware entirely** with a build that is authenticated and encrypted end-to-end by construction, integrates cleanly with Home Assistant through the existing ESPHome + Native API stack, and does not depend on operators remembering to turn on IP allow-lists or wrap insecure ports in tunnels.

**Approach.** This document describes a firmware and Home Assistant integration architecture for network-attached Zigbee and Thread coordinators built on an ESP32-S3 host with UART-connected radio MCUs.

Initial target hardware: **SMLIGHT SLZB-MR4U** (ESP32-S3 + Ethernet, TI CC2674P10 Zigbee radio, Silicon Labs EFR32MG26 Thread radio). The architecture must remain generic enough to apply to other ESPHome-compatible coordinators.

The central idea is to replace unencrypted raw TCP-to-UART ports with an **authenticated, encrypted serial transport using the ESPHome Native API**, while preserving compatibility with Home Assistant ZHA (Zigbee) and the Home Assistant OTBR add-on (Thread).

No new cryptographic protocol will be invented if existing ESPHome functionality (Noise `NNpsk0` + ChaCha20-Poly1305) suffices.

**Scope note.** Transport is a **build-time (config-time) choice** — each firmware image selects exactly one of `ethernet`, `wifi`, or `usb`. Concurrent transports are not supported. `ethernet` and `wifi` share the same encrypted ESPHome Native API design (§5–§24); they differ only in the link layer and SMLIGHT's `packages/wifi/` is reusable as-is for the Wi-Fi variant. The `usb` variant is a separate deployment mode with different security properties — see §25. Pre-existing USB coordinator products (SLZB-06/07, generic CC26xx/EFR32 sticks) are already local `/dev/ttyUSB*` devices consumed directly by ZHA/OTBR and are outside the scope of this document.

---

## 2. Problem statement

The existing SMLIGHT firmware exposes each radio UART as an unencrypted TCP `stream_server` and the management surface as plain HTTP:

```text
TCP 7638 -> CC2674 (Zigbee), unencrypted stream
TCP 6638 -> EFR32  (Thread), unencrypted stream
TCP 8638 -> external USB passthrough (optional), unencrypted stream
TCP 80   -> HTTP management (no TLS)
```

Concrete problems:

- **Serial ports (`:6638` / `:7638` / `:8638`)**: any host on the LAN that can reach these ports can inject serial data, disrupt the active controller, observe traffic, or drive the radio bootloader. The only mitigation offered by SLZB-OS is an operator-configured source-IP allow-list, which is off by default.
- **Web UI (`:80`)**: served over plain HTTP. Admin passwords (used to change radio modes, flash IEEE addresses, enter the bootloader, configure VPN, etc.) and any session cookies cross the LAN in cleartext on every login.
- **No application-level authentication or encryption** between coordinator and host software. Firewalling can hide reachability but cannot make the underlying protocol trustworthy.

Firewalling and IP allow-lists mitigate reachability, but the design goal is stronger:

> All communication between the coordinator and its host software MUST be authenticated and encrypted at the application/device level. Raw TCP UART ports MUST NOT exist on the network at all, and the management surface MUST NOT depend on cleartext credentials.

---

## 3. Security objective

Desired MR4U network surface (`ethernet` / `wifi` builds):

```text
TCP 6053   ESPHome Native API   (encrypted — pre-shared key; details below)
TCP 3232   ESPHome OTA          (encryption inherits API PSK — same key)
UDP 5353   mDNS discovery       (standard ESPHome, advertises host + services on link-local)
TCP 6638   CLOSED               (was: EFR32 stream_server, plaintext)
TCP 7638   CLOSED               (was: CC2674 stream_server, plaintext)
TCP 8638   CLOSED               (was: USB pass-through stream_server, plaintext)
TCP 80     CLOSED               (was: SLZB-OS HTTP admin UI)
```

The three ports that remain open are the standard ESPHome trio:

- **`:6053/tcp` (Native API)** — every operator-facing call (sensor state, service invocations, `serial_proxy` byte streams, log tail) travels here, mutually authenticated by the pre-shared `device_encryption_key`. Handshake is Noise `NNpsk0`; symmetric cipher is ChaCha20-Poly1305.
- **`:3232/tcp` (OTA)** — firmware uploads are encrypted with the same PSK (bare `ota: encryption:` block in `packages/core/core.yaml` inherits the API key). ESP32's default OTA port; ESP8266 uses `:8266`, which is where a lot of published examples still show. No cleartext image ever crosses the network; no separate OTA password to leak.
- **`:5353/udp` (mDNS)** — link-local discovery so the ESPHome dashboard and HA integration can find the device. Advertises hostname and service names only; carries no secrets. Standard behaviour for every ESPHome node.

Threats in scope:
- LAN attacker performing port scans, packet capture, replay, or malformed protocol packets on the radio streams.
- Unauthorized attempt to take over the serial stream.
- **Credential capture on the management path** (the SLZB-OS HTTP UI leaks admin passwords and session cookies in cleartext on every login; our design has no such path).

Out of scope:
- Physical access to the MR4U, flash extraction, compromised HA host, compromised ESPHome build pipeline, malicious radio firmware.

Encryption is not a replacement for network segmentation — normal firewalling remains recommended (defense in depth).

---

## 4. Hardware architecture

```text
                +----------------+
Ethernet -------|    ESP32-S3    |
  (or Wi-Fi)    | UART 1         |------ CC2674P10  (Zigbee)
                | UART 2         |------ EFR32MG26  (Thread RCP)
                | GPIO           |------ radio reset / boot lines
                | USB-C          |------ ESP32-S3 native USB (see §25)
                +----------------+
```

The ESP32-S3 is a network↔serial bridge. Zigbee and Thread protocol logic run on the dedicated radio MCUs. The MR4U ships with Ethernet + PoE; Wi-Fi is available on the same MCU and uses the same encrypted transport with no architectural changes.

---

## 5. Target architecture

```text
                        HOME ASSISTANT
                ┌──────────────────────────┐
                │   ZHA                    │
                │    │                     │
                │  serialx                 │
                │    │  esphome-hass://    │  (in-process)
                │    │                     │
                │   OTBR add-on            │
                │    │                     │
                │  serialx  esphome://     │  (standalone, own API session)
                │    │                     │
                │  PTY /tmp/ttyOTBR        │
                │    │                     │
                │  otbr-agent              │
                └──────────┬───────────────┘
                           │
                  ESPHome Native API
                    Noise + PSK
                           │
                    ENCRYPTED LAN
                           │
                ┌──────────▼───────────────┐
                │          MR4U            │
                │       ESP32-S3           │
                │    ESPHome API           │
                │      /       \           │
                │ serial_proxy  serial_proxy│
                │    │             │       │
                │  UART1         UART2     │
                │    │             │       │
                │ CC2674P10    EFR32MG26   │
                │  Zigbee        Thread    │
                └──────────────────────────┘
```

Only the encrypted ESPHome Native API crosses the physical LAN. No `stream_server`, no web UI, no MQTT on the ESP32.

---

## 6. Key architectural insight

Home Assistant Core ships `serialx`, a URL-to-transport router with a first-class `esphome-hass://` scheme. Any HA-side consumer that opens a serial device through `serialx` reaches an ESPHome `serial_proxy` port over the encrypted Native API without touching pyserial. The ZHA and OTBR paths in this design both ride that same transport.

See [integration-recipes.md](../integration-recipes.md) §1 for both URL schemes (`esphome-hass://` inside HA Core, `esphome://` from separate containers) and their operational consequences.

---

## 7. ESPHome Native API encryption

ESPHome's Native API supports Noise `NNpsk0` with ChaCha20-Poly1305 AEAD. Message type, length and protobuf payload are all inside the authenticated encrypted envelope.

```yaml
api:
  encryption:
    key: !secret device_encryption_key
```

The PSK is a device credential and MUST be treated as such. OTA reuses the same key — the bare `encryption:` block below inherits it (ESPHome 2026.9+), so a single per-device PSK protects both transports:

```yaml
ota:
  - platform: esphome
    encryption:
```

No custom TLS, nonce handling or replay-protection code will be written.

Both HA Core and the OTBR add-on will use the same device PSK — ESPHome Noise does not currently support per-client credentials. This is acceptable for v1.

---

## 8. ESPHome firmware structure

Start from the existing repository:

```text
smlight-tech/slzb-esphome   (GPL-3.0, upstream MR4U hardware definitions)
```

The delta should stay small.

| Keep                                       | Remove              | Add                            |
|--------------------------------------------|---------------------|--------------------------------|
| MR4U board / pin definitions               | `stream_server`     | `api` with `encryption`        |
| Ethernet, OTA, diagnostics                 | `web_server`        | `serial_proxy` × 2 (CC26 / EFR32) |
| Radio reset & boot GPIOs                   | Raw TCP UART ports  | Authenticated reset/bootloader actions |
| MRxU abstractions                          | BLE (initially)     |                                |

Do not fork ESPHome Core. If `serial_proxy` needs improvements (buffering, HW flow control), attempt them upstream first.

### 8.1 Upstream repositories

Three upstream repositories are relevant. Development starts with the first; the other two are only touched if needed.

| # | Repository | Role | Expected modification |
|---|------------|------|-----------------------|
| 1 | [`smlight-tech/slzb-esphome`](https://github.com/smlight-tech/slzb-esphome) | MR4U hardware definitions and ESPHome build targets. MR4U target: `mr4u-r1-73.yaml`. | **Fork.** Remove `stream_server` / `web_server`; add encrypted `api` + two `serial_proxy` instances; keep hardware definitions upstream-tracked. |
| 2 | [`home-assistant/core`](https://github.com/home-assistant/core) | ZHA, ESPHome integration, `serialx` registration. | **No changes required.** §10 validation confirmed no layer bypasses `serialx`; the stock chain works end-to-end. |
| 3 | [`home-assistant/addons`](https://github.com/home-assistant/addons) | The OpenThread Border Router add-on. | **Prototype the `serialx esphome:// → PTY → otbr-agent` adapter here** (§11). |

Development ordering:

```text
smlight-tech/slzb-esphome        ← START HERE
        │
        └── our fork
             └── secure MR4U firmware  (Phases 1–3)

home-assistant/core              ← ZHA validated against firmware; no patches needed (Phase 2 done)

home-assistant/addons            ← OTBR ESPHome-serial adapter prototype (Phase 4)
```

Get the MR4U booting with two encrypted `serial_proxy` instances and passing `nmap` (Phase 1) before touching either Home Assistant repository.

### Illustrative YAML (exact pins from upstream hardware definitions)

```yaml
api:
  encryption:
    key: !secret device_encryption_key

ota:
  - platform: esphome
    encryption:

uart:
  - id: efr32_uart
    tx_pin: ...
    rx_pin: ...
    baud_rate: 460800

  - id: cc2674_uart
    tx_pin: ...
    rx_pin: ...
    baud_rate: 115200

serial_proxy:
  - id: sp_uart1
    uart_id: uart1
    name: "zigbee"      # port_name in the client URL; CC2674P10 by default (SMLIGHT MR4U)

  - id: sp_uart2
    uart_id: uart2
    name: "thread"      # EFR32MG26 by default
```

Explicitly absent: `stream_server`, `web_server`, `mqtt`.

**Port name convention.** The `port_name=<x>` in the `serialx` URL matches the `name:` field of the corresponding `serial_proxy` (not its `id:`). This design names ports by the **default protocol role** (`zigbee`, `thread`, `zwave`, `usb`) rather than by chip. The rationale: the vast majority of users never re-purpose a radio, and role-based names give an operator-meaningful URL out of the box (`?port_name=zigbee` reads better than `?port_name=cc2674` for the 99% case). An operator who does swap radio duties (e.g. reflashes the CC2674 as a Thread router) is expected to edit the `name:` in their fork of the device YAML to stay semantically accurate; the shipped `serial_proxy` `name:` is a config value, not a chip binding. See §29.1 for URL examples.

---

## 9. Zigbee integration (ZHA)

ZHA is the v1 primary integration target. The transport chain is: ZHA → zigpy → protocol-specific radio backend (`zigpy-znp` for CC2674, `bellows` for EFR32) → `zigpy.serial` → `serialx` → `esphome-hass://` → `serial_proxy` on the device. No component in that chain is host-side compatibility glue — every layer is stock upstream.

See [integration-recipes.md](../integration-recipes.md) §2 for the ZHA pairing flow, the ZNP/EZSP wire-protocol context, Z2M as a future client (paths A and B), and the anti-features list.

---

## 10. ZHA validation (completed)

Not every integration reliably reaches `serial_proxy` through `serialx`; some historically call pyserial directly. Empirical validation against physical MR4U hardware confirmed the whole chain is clean:

1. **ZHA opens the coordinator via `serialx`.** Both zigpy backends (`zigpy-znp` for CC2674, `bellows` for EFR32) go through `zigpy.serial` → `serialx` → `esphome-hass://<entry_id>?port_name=zigbee`. No pyserial bypass.
2. **The CC2674-compatible zigpy library propagates the transport correctly.** No direct pyserial call.
3. **`esphome-hass://…` opens without URL rewriting.** `CONF_DEVICE_PATH` is copied verbatim from the config entry into the backend controller (see [`archive/zha-zigpy-inspection.md`](archive/zha-zigpy-inspection.md) §2.4).
4. **Baudrate and flow-control requests propagate.** `bellows`'s default `xon_xoff=True` is a no-op through the ESPHome transport, as expected; HW flow control is set at the device via `uart*_hw_flow` (§12).
5. **ESPHome API session reconnect** cleanly closes and reopens the port on the HA side; ZHA reconnects the coordinator without user intervention.

Outcome: **no HA Core, `zigpy`, or `serialx` patches were required.** Full pairing, binding, group traffic, and OTA all work over the encrypted transport.

---

## 11. Thread integration (OTBR)

`otbr-agent` only accepts `spinel+hdlc+uart://<posix-path>` URLs — it does not speak `serialx` natively. Our path is a small Python adapter, running inside the HA OTBR add-on's own container, that opens `esphome-hass://` via `serialx`, creates a PTY via `os.openpty()`, symlinks it to `/tmp/ttyOTBR`, and forwards bytes bidirectionally. `otbr-agent` sees a normal POSIX serial device; the encryption and transport are transparent to it.

See [integration-recipes.md](../integration-recipes.md) §3 for the Spinel primer, adapter architecture, add-on config surface, supervised-failure lifecycle, and Thread/Matter commissioning flow.

---

## 12. Serial flow control

Baseline (matches the SLZB-OS **Serial options** page, which ships with HW flow control **disabled** on both radios and warns: *"Only enable if the Zigbee/Thread firmware release notes confirm HW flow control support; otherwise leave disabled"*):

```text
CC2674P10: 115200, no HW flow control
EFR32MG26: 460800, no HW flow control
```

The MR4U hardware wires RTS/CTS between the ESP32-S3 and each radio (the Dashboard's "HW & SW" field advertises the capability, not the current setting), but whether the *radio-side* firmware honors HW flow control depends on the specific EmberZNet / Z-Stack build. Ship with HW FC **off** by default, matching SLZB-OS, and enable it per-radio only when:

1. the radio firmware release notes explicitly confirm HW FC, **and**
2. stress testing shows dropped frames or instability without it.

Do **not** enable software (XON/XOFF) flow control — Spinel and Zigbee framing are binary and will collide with 0x11/0x13.

`serial_proxy` accepts a `flow_control` parameter and exposes `rts_pin`/`dtr_pin` as modem-control outputs, but transparent RTS/CTS across ESPHome/ESP-IDF is not currently plumbed by the upstream `uart:` component. Phase 1 ships `components/uart_hw_flow/` — a ~20 LOC fork-local external component that calls ESP-IDF `uart_set_hw_flow_ctrl(UART_HW_FLOWCTRL_CTS_RTS, …)` on the configured UART, controlled per-UART via the `uart*_hw_flow: true|false` substitution in `hw_defs/**`. Current MR4U radios keep it `false` (matches SMLIGHT firmware defaults); the axis exists so future radios that require HW FC (e.g. MG24 Thread) work by config alone. An upstream ESPHome PR adding `cts_pin`/`rts_pin` to `uart:` remains the preferred long-term landing; the external component is the interim carrier. See [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §5.

---

## 13. Reliability & buffering

`serial_proxy` currently forwards up to 256 received UART bytes per loop iteration. This must be tested under sustained Thread/RCP traffic.

Required behavior:
- Bounded buffers with clear backpressure — no unbounded queues.
- Never silently drop bytes.
- Automatic recovery from: Ethernet interruption, ESP32 reboot, HA restart, ESPHome OTA reboot, add-on restart, radio reset, prolonged idle, burst traffic.
- On ESPHome API loss → close the local endpoint (PTY) immediately; do not pretend the radio is still connected.
- On client disconnect → release ownership; flush stale outgoing bytes so they are not replayed to the next client.

Metrics to expose (per radio):

```text
bytes_rx, bytes_tx
buffer_high_watermark, dropped_bytes
reconnect_count, api_disconnect_count, uart_errors
```

`dropped_bytes` must remain 0 under all supported workloads.

---

## 14. Ownership semantics

Only one application may own a given physical radio at a time. `serial_proxy` already tracks the subscribing API connection as the port owner. The Home Assistant side must preserve this — attempting to open a radio that is already owned should fail cleanly (`connection refused` / port busy), not silently share the stream. A short reconnect grace period is useful during application restarts.

---

## 15. Radio management & maintenance

Radio reset, bootloader entry, and other radio-touching operations must be exposed as **authenticated ESPHome Native API actions**, never as raw GPIO switches or unauthenticated HTTP endpoints:

```text
zigbee_radio_reset               thread_radio_reset
zigbee_radio_bootloader          thread_radio_bootloader
set_radio_baudrate
zigbee_ieee_read                 zigbee_ieee_write        # coordinator migration
zigbee_channel_energy_scan                                # clean-channel diagnostic
```

Bootloader entry, IEEE flashing, and channel scan must each be distinct action/button entities, not plain switches.

**IEEE address management** is essential for migration: reading the current IEEE from the outgoing coordinator and writing it to the new one lets existing Zigbee devices reconnect without re-pairing (see §29).

**Ownership rule for radio-touching actions**: SLZB-OS's own UI warns that *"IEEE read/write or Zigbee network scan will break your existing Z2M/ZHA connection."* The same applies here — the radio UART cannot simultaneously carry a live NCP session and out-of-band management traffic. Our actions must:

- **Refuse** if a client currently owns the `serial_proxy` port, returning a clear error ("stop ZHA/OTBR first"), **or**
- **Force-release** the port (close the subscribing API connection) before executing, and require the client to reconnect afterward.

Pick one policy consistently; document it. Do not silently interleave management traffic with client traffic.

Useful diagnostic entities to expose (SLZB-OS Dashboard equivalents, all free via ESPHome sensors):

```text
uptime, ESPHome version, Ethernet/Wi-Fi status
CC2674 / EFR32 status, radio firmware versions
serial connection state, error counters
SoC temperature (ESP32-S3), per-radio temperature (if the NCP exposes it)
```

Note: Zigbee radio **TX power** is a Zigbee-stack / NCP-side setting configured by ZHA or Z2M (e.g. Z2M's `advanced.transmit_power`), not by our firmware. Do not add a firmware-side TX-power control.

---

## 16. Radio firmware updates (future)

The same encrypted transport should later carry firmware updates:

```text
firmware image -> flashing tool -> ESPHome Native API -> ESP32 -> radio bootloader
```

- CC26xx: existing SMLIGHT tooling may be reusable.
- EFR32: Silicon Labs bootloader tooling must be investigated.

Not part of v1. Prove runtime serial transport first.

---

## 17. Discovery, logging, hardening

- **mDNS**: keep enabled for convenience in v1 (API is still encrypted). A hardened mode with mDNS disabled and static IP is an option.
- **Logging**: never log raw UART payloads, Spinel/Zigbee bytes, PSKs, OTA passwords, or HA tokens. Log connection state, reconnects, byte counters, errors, radio resets, firmware versions.
- **Firewall**: allow `MR4U:6053` only from the HA host if practical.

---

## 18. Non-goals for v1

- MQTT broker on the ESP32
- Zigbee2MQTT support in v1 (architecturally compatible as a future client — see §9.2 — but not shipped or validated in v1)
- Any public TCP serial port (raw or "on localhost")
- General-purpose external proxy daemon
- Custom cryptography or TLS
- Browser web UI on the MR4U
- **Runtime radio role-switching** ("turn Radio 1 into a router with one click"). Role is a compile-time property of the flashed radio firmware image; changing it requires editing the device YAML, reflashing the radio, and rebuilding the ESPHome image. See §24 and [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §6.
- **Radio-side sensors that require a persistent second protocol channel on the UART** — anything beyond the one-shot version probe at boot (§16). SLZB-OS surfaces radio SoC die temperature via ZNP/Spinel MFG-INFO queries, so it is technically obtainable; continuously polling would violate the `serial_proxy` dumb-pipe principle (§14) by requiring the firmware to arbitrate the UART against the paired HA client. Re-evaluate if a scheduled break-glass query pattern is ever added.
- Bluetooth proxy **enabled by default** (capability retained in firmware but disabled — see §27.3)
- Automatic radio firmware flashing
- Cloud dependency (including SLZB-OS-style cloud firmware-update checks and VPN)
- Multi-client sharing of a coordinator
- Modifying OpenThread itself to speak `esphome://`
- **On-device OTBR** (SLZB-OS "Thread+OTBR running on device (beta)") — border-router responsibilities stay in a supervised HA add-on
- **Matter-over-Thread endpoint mode** on the gateway itself — HA brokers Matter via OTBR
- **Zigbee Hub** / any on-device Zigbee application layer
- **USB-to-Ethernet passthrough** for external USB dongles plugged into the MR4U's USB host port (SLZB-OS TCP :8638) — same insecure category as :6638/:7638; if wanted later, expose via `serial_proxy` on the encrypted Native API, not raw TCP
- **On-board microphone as an HA entity, and sound-reactive WS2812 effects (`music_leds` + `fastled_helper`)** — upstream ships these via the third-party [`andrewjswan/esphome-components`](https://github.com/andrewjswan/esphome-components) source tree. Removed from this fork along with the FastLED library dependency. The FFT + AGC + peak-detection task on the second core + WS2812 RMT output competes with `serial_proxy`'s radio UART loops for CPU / interrupt budget, producing intermittent byte drops on 115200–460800-baud Zigbee / Thread / Z-Wave streams. Attack-surface reduction is the fork's headline motive; **radio-transport reliability** is its second, non-negotiable pillar — anything on the SoC that jeopardizes it fails the ship criterion by construction. If sound-reactive effects ever return, they will be gated behind an explicit runtime "pause radios while mic active" switch, not on-by-default.

---

## 19. Phased implementation plan

The Phases below are the architectural milestones this document builds toward. [roadmap.md](roadmap.md) is organized by shipped user-facing version (v1, v1.x, v2) rather than by Phase, since one Phase can span multiple releases and one release can advance multiple Phases. The two views are complementary; roadmap.md is the authoritative source for what ships when.

### Status snapshot

| Phase | State | Notes |
|---|---|---|
| Phase 0 — Upstream inspection | ✅ Done | commit `7c507be`; ground-truth references in [`docs/design/archive/`](archive/) |
| Phase 1 — Secure firmware (repo-wide) | ✅ Done | commits `307fd30`, `3a0ebef` — pushed to `origin/secure-native-api`. Empirical hardware verification carried under Phase 2. |
| Phase 2 — Native ZHA validation | ✅ Done | End-to-end chain verified on MR4U; see §10. No HA Core / `zigpy` / `serialx` patches required. |
| Phase 3 — Zigbee reliability testing | ⏳ Pending | |
| Phase 4 — OTBR internal PTY adapter prototype | ⏳ Pending | |
| Phase 5 — Thread reliability testing | ⏳ Pending | |
| Phase 6 — Reliability diagnosis | ⏳ Conditional | Only if Phase 3/5 shows problems |
| Phase 7 — Native radio-maintenance actions | ⏳ Pending | |
| Phase 8 — Encrypted firmware flashing | ⏳ Pending | |
| Phase 9 — USB build variant (post-v1) | ⏳ Pending | Gated on §26.3 |

For the current release plan and active work items organized by shipped version, see [roadmap.md](roadmap.md).

---

## 20. Testing matrix (minimum)

| Test                        | Zigbee | Thread |
|-----------------------------|:------:|:------:|
| Basic connection            |   ✓    |   ✓    |
| 24 h operation              |   ✓    |   ✓    |
| 7-day soak                  |   ✓    |   ✓    |
| ESP32 reboot                |   ✓    |   ✓    |
| HA / host reboot            |   ✓    |   ✓    |
| Ethernet interruption       |   ✓    |   ✓    |
| API reconnect               |   ✓    |   ✓    |
| Client (ZHA/OTBR) restart   |   ✓    |   ✓    |
| OTA ESP32 update            |   ✓    |   ✓    |
| High traffic                |   ✓    |   ✓    |
| Invalid PSK                 |   ✓    |   ✓    |
| Unauthorized LAN client     |   ✓    |   ✓    |
| Stale Thread route check    |   —    |   ✓    |

Performance targets:

```text
silent dropped bytes        0
unauthenticated access      impossible
raw LAN UART ports          0
automatic reconnect         yes
```

---

## 21. Empirical unknowns

Several behaviors can only be resolved with hardware under load: dropped-byte behavior at sustained line rate, HW flow-control interaction with each radio's firmware, buffer sizing across the Native API pipeline, reconnect latency after Ethernet interruption, and stale-Thread-route cleanup after a forced ESPHome disconnect. Each is a gate on the Phase 3 (Zigbee reliability) and Phase 5 (Thread reliability) acceptance criteria in [roadmap.md](roadmap.md).

---

## 22. Guidance for future contributors

- Do not assume ESPHome APIs from memory — inspect the current source and docs. `serial_proxy` is experimental.
- Do not invent ESPHome YAML options that do not exist.
- Do not implement custom cryptography unless the ESPHome Native API proves unusable.
- Prefer upstream contributions over private forks and over host-side compatibility wrappers.
- The two `serialx` URL schemes exist for a reason — use `esphome-hass://` inside HA Core, `esphome://` from separate containers (OTBR add-on).
- Preserve the security invariant: no raw radio UART on the LAN, ever.
- Never expose the OTBR internal PTY as a network endpoint.
- Validate reconnect and packet-loss behavior experimentally, not by inspection.
- Radio flashing stays out of the critical path until normal operation is stable.

---

## 23. Success criterion

> Home Assistant operates both the MR4U Zigbee coordinator (via ZHA) and Thread RCP (via the OTBR add-on) through ESPHome's authenticated encrypted Native API, with no unencrypted radio interface on the physical network and no general-purpose external proxy daemon.

Preferred end-user experience:

```text
Install secure MR4U firmware
        ↓
Add ESPHome device to HA (PSK)
        ↓
Select the Zigbee radio in ZHA   (default: CC2674)
        ↓
Point OTBR add-on at MR4U + PSK + port_name of the Thread radio
        ↓
Done
```

Encryption, serial transport, PTY adaptation and radio routing remain implementation details.

---

## 24. Architectural principle

> Do not wrap an insecure exposed port in a secure tunnel. Remove the insecure port entirely.

The radio UART terminates inside the ESP32-S3 firmware. The only network path to it is an authenticated encrypted ESPHome Native API session. Any file-descriptor adaptation (the OTBR PTY) lives inside the consuming application's container and never touches the network.

### 24.1 Design trajectory: shedding glue over time

The design is a **local optimum** given its constraints: HA is the host, ESPHome is the firmware framework, no new crypto, no new integrations. Within those constraints, the transparent-encrypted-pipe topology is as small as it gets — every alternative (decode-and-re-expose, on-device stack, Matter bridge) widens the interface and turns us into a Zigbee/Thread stack vendor. This is not the *global* optimum for every conceivable Zigbee/Thread product; it is the optimum for "secure MR4U for HA users, minimal invention, minimal plugin surface."

**The narrow-waist principle.** Serial-byte tunneling looks like extra machinery only if compared against a fantasy where the host↔NCP split doesn't exist. Compared against realistic alternatives it is dramatically the smallest interface. It is:

- Radio-firmware agnostic — upgrade CC2674 EmberZNet or EFR32 OpenThread without touching our firmware
- Host-integration agnostic — ZHA today, Z2M tomorrow (§9.2), custom test tools always
- Cryptographically bounded by one Noise session — no new attack surface
- Not our invention — `serial_proxy` + `esphome-hass://` is stock ESPHome + stock HA

Widening the interface (e.g. decoding Spinel/ZNP on the ESP32 and re-exposing higher-level events) means every zigpy quirk update or OpenThread revision becomes an ESPHome firmware release. That is a much worse deal than tunneling bytes.

**Aspirational end-state: zero HA-side glue.** The "finished" version of this design has *zero* patches or adapters outside our own ESPHome firmware fork. **One glue point remains in v1**, with a concrete upstream retirement path:

| Glue in v1 | What retires it | Section |
|---|---|---|
| OTBR add-on internal PTY adapter | Upstream `spinel+hdlc+esphome://` scheme in OpenThread | §11.4 |

(The originally-anticipated second glue point — a zigpy backend shim in case a radio backend bypassed `serialx` — turned out to be unnecessary. §10 validation confirmed the full ZHA → zigpy → `serialx` chain is clean for both radios, with no `pyserial` bypass. No shim was ever needed and none will be shipped.)

The day this design ships with zero HA-side patches — stock ESPHome integration, stock ZHA, stock OTBR add-on, stock Thread integration — is the day it is finished. Everything until then is bridging code we are actively trying to delete.

**Push-further point: land `spinel+hdlc+esphome://` upstream (see §11.4).**

OpenThread's radio-URL syntax is pluggable — schemes like `spinel+hdlc+uart:///dev/ttyUSB0` and `spinel+hdlc+forkpty:///path/to/program` are each a small C++ class in `openthread/src/posix/platform/`. Adding an `esphome://` scheme means writing one such class that:

1. Negotiates a Noise session against ESPHome's Native API on open
2. Subscribes to the `serial_proxy` for the configured `port_name`
3. Feeds RX bytes into HDLC framing and pumps TX bytes out
4. Handles reconnect

With this in place, `otbr-agent` config becomes literally one line: `RADIO_URL=spinel+hdlc+esphome://mr4u:6053/?port_name=thread`. The §11 PTY adapter, its supervision logic, and its config surface all vanish. Every other user of a remote encrypted RCP benefits too, so it's a real upstream contribution, not a hostile patch we push for our own use. Non-trivial C++ in a security-sensitive codebase and OpenThread's release cadence is slow — hence "future," hence the pragmatic PTY bridge in v1.

**North star.** Every shim retired is a plugin we said we didn't want and now don't have. §10 already delivered one such retirement (no zigpy shim); the OTBR upstream landing above is the last one on the v1 horizon.

---

## 25. USB build variant (deferred, post-v1)

Transport is a **build-time choice**, not a runtime switch. Each firmware image selects one of:

| Build | Transport | Design coverage |
|-------|-----------|-----------------|
| `ethernet` | Native API over Ethernet, Noise + PSK | §5–§24 (primary design) |
| `wifi` | Native API over Wi-Fi, Noise + PSK | §5–§24 (same design, different link) |
| `usb` | USB CDC-ACM directly to host | this section |

The `usb` build does **not** run the encrypted Native API, `serial_proxy`, or any networking stack. There is no `api`, `ethernet`, or `wifi` component in the image. It is a distinct, smaller firmware whose only job is to expose each radio as a virtual serial port over USB-C.

Because the transports are mutually exclusive, no ownership arbitration or transport-switching logic is needed.

The hardware itself does *not* forbid concurrent USB + network operation — SLZB-OS exposes a "Keep ON Wi-Fi/Ethernet network & web server in USB mode" toggle that runs both. Our mutex is a **firmware policy** chosen for UX simplicity, smaller attack surface, and testability. Revisit only if a compelling user story emerges post-v1.

### 25.1 Target user experience

The MR4U in `usb` build must behave like a native Home Assistant Zigbee/Thread antenna — analogous to Home Assistant SkyConnect / Connect ZBT-1 / the built-in radio on HA Yellow/Green:

1. User plugs MR4U into HA host via USB-C.
2. HA USB discovery fires.
3. ZHA offers the Zigbee CDC endpoint as a coordinator candidate.
4. OTBR (or HA's Thread integration) offers the Thread CDC endpoint.
5. User picks each and it just works.

No Native API dance, no PSK, no add-on configuration — identical to plugging in a SkyConnect.

### 25.2 Availability upstream (checked)

- **ESPHome upstream**: no ready-made component for a composite USB-device with multiple CDC endpoints bridged to internal UARTs. Native USB-CDC exists only for `logger` and JTAG programming.
- **SMLIGHT `slzb-esphome`**: `packages/usb/usb_uart.yaml` does the *opposite direction* (ESP32-S3 as USB **host** talking to an external CP210x bridge chip, re-exposed via `stream_server` on TCP 9638). There is no USB-device-mode CDC bridge in that repo. *(In this fork we have already converted that file to `serial_proxy` so a future USB-host variant is secure by default; it is still not a USB-**device**-mode CDC bridge.)*

**Verdict: not reusable ready-made from either upstream.** A custom ESPHome external component is required.

### 25.3 Implementation sketch

- ESP-IDF `tinyusb` composite USB device with **two CDC-ACM interfaces** on the ESP32-S3 native USB peripheral.
  - CDC 0 → UART1 → CC2674P10 (enumerates as `/dev/ttyACM0`)
  - CDC 1 → UART2 → EFR32MG26  (enumerates as `/dev/ttyACM1`)
- Bidirectional byte forwarding per endpoint, matching baudrate/flow-control conventions of the radio.
- Radio reset / bootloader GPIOs remain controllable, either via CDC modem-control lines (DTR/RTS) mimicking the reference dongle behavior, or via a small vendor request.

### 25.4 USB descriptor compatibility (the actual hard part)

Auto-discovery in HA is descriptor-driven. Two files matter (verified against `home-assistant/core` `dev` branch):

- `homeassistant/generated/usb.py` — the master `(VID, PID, description*, manufacturer*)` → integration domain table. Auto-generated by hassfest from each integration's manifest.
- Per-integration `manifest.json` `"usb": [...]` blocks that feed into the table above.

Concrete relevant entries today:

| Device | VID | PID | Description match | Routes to |
|---|---|---|---|---|
| SkyConnect v1.0 | `0x10C4` | `0xEA60` | `*skyconnect v1.0*` | `homeassistant_sky_connect` |
| HA Connect ZBT-1 | `0x10C4` | `0xEA60` | `*home assistant connect zbt-1*` | `homeassistant_sky_connect` |
| HA Connect ZBT-2 | `0x303A` (Espressif) | `0x4001` or `0x831A` | `*zbt-2*` | `homeassistant_connect_zbt2` |
| SMLIGHT SLZB-07 | `0x10C4` | `0xEA60` | `*slzb-07*` | `zha` |
| slae.sh CC2652 | `0x10C4` | `0xEA60` | `*2652*` | `zha` |
| SONOFF Dongle Max MG24 | `0x10C4` | `0xEA60` | `*sonoff*max*` | `zha` |

Two important gaps to understand before writing any descriptor code:

**Gap 1 — OTBR/Thread is not in the USB table.** The `otbr` integration's `manifest.json` has **no** `"usb": [...]` block. Thread auto-discovery is not driven by the USB matcher directly; it is driven through the **hardware-wrapper integrations** (`homeassistant_sky_connect`, `homeassistant_connect_zbt2`, `homeassistant_yellow`, `homeassistant_hardware`). Those wrappers own the "this device provides both Zigbee and Thread" logic and hand off to ZHA and OTBR internally.

**Gap 2 — Every currently recognized dongle is single-radio.** All entries in the USB table are single-radio products (one EFR32 doing Silicon Labs multiprotocol, or one CC26xx doing Zigbee only). HA has no existing schema for "one USB device presenting two independent radio endpoints."

Because of these gaps, three implementation paths exist \u2014 in increasing order of correctness:

1. **Emulate two independent dongles behind an internal USB hub.** TinyUSB configures the ESP32-S3 as a composite `hub + 2× CDC devices`. Each CDC device presents distinct descriptors matching an existing profile (e.g. one as `*slzb-07*` for the CC2674 endpoint, one as `*zbt-2*` for the EFR32 endpoint). Quickest to prototype; ethically questionable and fragile against upstream matcher changes.
2. **Single composite device with new descriptors + upstream a new "MR4U" hardware wrapper.** Pick `VID 0x303A` (Espressif) with unassigned PIDs, use description strings like `*mr4u zigbee*` and `*mr4u thread*`, and submit a `homeassistant_mr4u` (or an MR4U entry inside `homeassistant_hardware`) PR to HA core that maps both endpoints to ZHA and OTBR respectively. Cleanest long-term; requires the PR to be accepted.
3. **Ship without auto-discovery**, requiring the user to manually select each CDC endpoint in ZHA and OTBR. Trivial to build, worst UX — defeats the "just like SkyConnect" goal.

Path (2) is the target. Path (1) is an acceptable prototype while (2) is in review. Path (3) is the fallback if upstream integration stalls.

**Coordinate with SMLIGHT.** SLZB-07 already has a matcher for `zha`; if SMLIGHT-adjacent (or a new dedicated MR4U profile) is upstreamed, avoid stepping on the SLZB-07 descriptor pattern.

### 25.5 Security properties

- No listening TCP ports on the device. Nothing on the LAN.
- Access requires physical possession of the USB cable.
- No Noise/PSK layer is required or applied — there is no network path to protect.
- The primary security invariant ("no plaintext radio UART on the LAN") holds trivially in the `usb` build: there is no LAN involvement.
- Physical access was already excluded from the §3 threat model, so the `usb` build's threat surface is strictly smaller than the `ethernet`/`wifi` builds.

### 25.6 HA integration validation checklist

1. Plugging in the `usb` build triggers HA USB discovery.
2. Zigbee CDC endpoint is offered as a ZHA coordinator candidate and initializes.
3. Thread CDC endpoint is offered to OTBR (or HA Thread integration) and initializes.
4. The two endpoints are never confused for one another.
5. USB re-plug, host reboot, ESP32-S3 firmware update all recover cleanly (matching SkyConnect behavior).
6. Radio reset / bootloader entry (§15) still works over USB.

### 25.7 Effort & recommendation

Moderate implementation effort: a custom ESPHome external component (a few hundred lines of C++/ESP-IDF TinyUSB code + UART bridging). The **descriptor / HA-discovery integration** is the larger uncertainty and should be prototyped against HA's actual discovery code before committing to a descriptor scheme. See §26 for the HA integration path — for the `usb` build this is the deciding factor on whether the feature is worth shipping.

**Recommendation:**

- Complete Phases 1–5 (encrypted `ethernet` / `wifi` transport, ZHA, OTBR) first.
- Treat `usb` as a **separate build variant** delivered as Phase 9 (post-v1).
- If pursued, prefer submitting the composite-CDC-over-UART component to ESPHome upstream (e.g. `usb_serial_bridge`) and pushing the MR4U USB profile to Home Assistant's discovery database rather than keeping either private.

---

## 26. Home Assistant integration path (per build variant)

SMLIGHT already has a first-class presence in HA, so the integration story is not "how do we get discovered from scratch." It is "where do our builds fit next to what already exists."

### 26.1 What currently exists upstream

- **`homeassistant/components/smlight/`** — a platinum-quality HA integration named "SMLIGHT SLZB", domain `smlight`, codeowner `@tl-sl` (SMLIGHT-affiliated), actively maintained.
  - Discovery: `zeroconf` `_slzb-06._tcp.local.` + `dhcp` with `registered_devices: true`.
  - Talks to SLZB-OS's HTTP/JSON API via `pysmlight` (the SMLIGHT Python library).
  - Provides device entities: diagnostics, radio reset buttons, firmware updates, buzzer/RTTTL, IR, BLE proxy, radio-type-aware naming.
  - Owns the handoff to ZHA for the Zigbee coordinator.
- **`homeassistant/components/zha/`** — already has upstream matchers for `slzb-06*` (mDNS) and `slzb-07*` (USB, VID `0x10C4` PID `0xEA60`).
- **`homeassistant/components/otbr/`** — no direct USB or mDNS matchers; Thread discovery goes through hardware-wrapper integrations (`homeassistant_sky_connect`, `homeassistant_connect_zbt2`, `homeassistant_yellow`).

**Consequence for our firmware.** SLZB-OS is proprietary. Our ESPHome firmware deliberately does not expose the SLZB-OS HTTP API (§13, §18). Therefore the existing `smlight` integration cannot manage a device flashed with our firmware — `pysmlight` will not find its endpoints. This is a conscious trade-off. The device management surface must come from somewhere else per build.

### 26.2 `ethernet` and `wifi` builds — no HA changes required

- The MR4U is added to HA as an **ESPHome device** via the standard ESPHome integration (encrypted API + PSK).
- Discovery is via ESPHome's own mDNS `_esphomelib._tcp.local.`, not `_slzb-06._tcp.local.`. The `smlight` integration correctly ignores it.
- All device-management entities (uptime, ESPHome version, radio reset, bootloader entry, firmware version, connection state, error counters — §15/§17) are exposed as ESPHome-native entities.
- ZHA reaches the Zigbee radio via `serialx` + `esphome-hass://` (§9).
- OTBR reaches the Thread radio via the add-on-internal `esphome://` + PTY adapter (§11).

**Zero upstream HA changes needed. Zero SMLIGHT coordination needed.** The trade-off is losing the SLZB-OS-only features (SLZB-OS scripting, HTTP UI, SMLIGHT cloud features) — which is the point of the alternative firmware.

### 26.3 `usb` build — three candidate paths

Path (a) is preferred; (b) is a viable fallback; (c) is prototype-only.

**Path (a) — extend the existing `smlight` integration** *(preferred)*

- Engage SMLIGHT (`@tl-sl`, `pysmlight` maintainer) with a proposal to either:
  - add an ESPHome-firmware-aware branch inside the `smlight` domain, or
  - create a companion `smlight_esphome` device integration under the same codeowner.
- Add USB matchers with VID `0x303A` (Espressif) + MR4U-specific PIDs and description strings like `*mr4u zigbee*` / `*mr4u thread*`.
- The `smlight`-family integration handles the HA-side device card, delegates Zigbee to ZHA and Thread to OTBR.
- Users see the familiar SMLIGHT device UI — best UX.
- **Requires SMLIGHT's cooperation and an accepted HA core PR.**

**Path (b) — independent discovery via ZHA + a new hardware wrapper** *(fallback)*

- Add MR4U USB entries directly to ZHA's `manifest.json` USB matcher list.
- For Thread, add a new `homeassistant_mr4u` hardware-wrapper integration (or an MR4U branch inside `homeassistant_hardware`) that offers the Thread CDC endpoint to OTBR.
- Skips the `smlight` umbrella — cleaner separation but presents the MR4U as a generic multi-radio dongle, not a SMLIGHT product. Two smaller PRs instead of one negotiation.
- Feasible without SMLIGHT's involvement.

**Path (c) — descriptor cloning** *(prototype only, never ship)*

- TinyUSB clones existing recognized dongle descriptors (per §25.4 option 1).
- Acceptable for internal testing during (a) or (b). Not for a public release: presents the device as something it is not and is fragile against upstream matcher changes.

### 26.4 Decision framework

| Question | If answer is… | Then… |
|---|---|---|
| Does the encrypted network transport meet the user need? | Yes | Ship v1 as `ethernet` + `wifi`; skip USB entirely. This is fully in-scope, zero upstream negotiation. |
| Is USB required as a first-class deployment? | Yes | Contact SMLIGHT and pursue Path (a). |
| Is SMLIGHT unresponsive or opposed? | Yes | Fall back to Path (b): two smaller upstream PRs, more work, worse UX. |
| Do both (a) and (b) stall? | Yes | **Cut `usb` from scope.** Document as "encrypted network only." |

### 26.5 Recommendation

- **v1: `ethernet` + `wifi` only.** No HA-side change required. No coordination required. Ship this first.
- **Post-v1: pursue Path (a) for the `usb` build.** Reach out to SMLIGHT before writing any TinyUSB code — their answer determines whether we invest.
- Do not release descriptor-cloned firmware publicly.
- If both integration paths stall, formally remove `usb` from the design and update this section to say so. That is an acceptable outcome; the primary user story is the encrypted network path.

### 26.6 Implementation shape: what needs Python, what stays in YAML

Where does "a HA-side integration" become necessary vs optional? The answer changes between v1 and v2, and this subsection makes that boundary explicit so we don't accidentally build more than we need or defer things that would in fact become cheap once we've committed to v2.

#### 26.6.1 v1 needs no Python integration

Every entity in §27.1's port list is either:

- **Native ESPHome** (`internal_temperature:`, `uptime:`, `restart:`, `button:`, `switch:`, `ethernet.connected`, etc.) — auto-discovered by HA's built-in ESPHome integration.
- **A ZNP-probe `text_sensor` published at boot** — same, auto-discovered.
- **A HA-side template entity** — the "radio firmware update" flow uses a `template update:` entity in HA's own YAML (or a shipped blueprint) that reads two REST sensors (installed_firmware, available_rev) and wires an `install` action to an ESPHome service call. Zero Python.

Concrete v1 shipping list on the HA side:

| HA-side artifact | What it does |
|---|---|
| `rest:` sensor polling `https://updates.smlight.tech/services/api/slzb-06x-ota.php?type=ZB&format=slzb` | Publishes the whole catalog as JSON attributes; template pulls `available_rev`, `available_link`, `available_notes` per radio |
| `template update:` entity | Combines `radio*_installed_firmware` (ESPHome) + filtered catalog entry (REST) into a HA update entity |
| Blueprint (optional) | Pre-wires the above for MR4U so users don't hand-write it |
| `shell_command:` (optional, for v1.5) | External flasher invocation, if a user wants the install action to actually flash |

**No `custom_components/` directory, no `manifest.json`, no config flow.** It's all YAML plus (optionally) an add-on the user installs from a documented URL.

#### 26.6.2 What v2 actually needs

v2's non-negotiable is **one-click Zigbee-radio firmware flash from HA**. The ESP32 cannot host the flasher binaries (silabs-firmware-flasher, cc2538-bsl.py, Python OT/Spinel tooling), so something HA-side must:

1. React to a HA event or service call.
2. Open TCP to a temporary raw-UART bridge port that ESPHome opens on demand (see [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §7 Option A).
3. Run the appropriate flasher against `socket://<esphome-ip>:<bridge-port>`.
4. Publish progress + result back into HA.

The lowest-effort shape that satisfies all four is a **Home Assistant add-on** (Docker container in the HA supervisor). Options in increasing complexity:

| Shape | Effort | User install path | Trade-offs |
|---|---|---|---|
| Documented `shell_command:` invoking a flasher binary the user installs manually | Very low (docs only) | Manual copy of a Python script | Fragile; no HA event pipeline; Windows/macOS HA users left out |
| **HA add-on published to a HACS repo** ← preferred | Medium | Add repo URL, install add-on | Runs in HA supervisor; can publish events; auto-updates; still no `custom_components/` needed |
| Full `custom_components/slzb_mr4u/` with add-on backend | High | HACS integration install | Nice service registration + Developer-Tools UX; more code to maintain |

We can defer choosing between "add-on only" and "add-on + custom_component" until v2.0 ships. Start with the add-on, add the thin custom_component wrapper only if users complain about UX.

#### 26.6.3 Which SLZB-OS features re-use v2's infrastructure at near-zero cost

Once we're paying for an add-on with Python + a ZNP client + a bridge to ESPHome, several previously-skipped items become cheap. The framework: **if it needs a Python-side ZNP/Spinel session, it belongs in v2. If it needs a persistent Zigbee client role, it belongs in Z2M/ZHA. If it needs a runtime on the device, it stays skipped.**

| SLZB-OS feature | v1 decision | v2 add-on decision | Reasoning |
|---|---|---|---|
| One-click radio flash | Skip | **v2.0 core** | The reason v2 exists |
| Post-flash re-probe | N/A | **v2.0 core** | Also the escape hatch that makes a `zbVer.txt`-style cache safe later (see [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §3 sidebar) |
| Flash progress events | Skip | **v2.0 nice-to-have** | Add-on emits `slzb.flash_progress` events; replaces SLZB-OS SSE `/events` pattern without needing SSE on ESP32 |
| Zigbee network backup (nwk key + PAN + device table export) | Skip | **v2.1 candidate** | Same ZNP session dumps to `/config/backups/`; parallel to HA snapshots, valuable for network-key rotation and disaster recovery |
| IEEE MAC read + migrate helper | Skip | **v2.2 candidate** | ZNP CMD 12/14 read, CMD 11 write. Real UX win for "I swapped a CC26xx" recovery |
| Radio SoC die temperature | v3+ | **v2.3 candidate** | ZNP `SYS_GET_MFG_INFO` (or Silabs equivalent) polled on the add-on's existing schedule. Publishes as HA sensor. Was v3+ under v1's constraints; v2 makes it v2.3. |
| Zigbee energy scan | Skip | **v2.4 candidate** | ZNP CMD 5 equivalent. Nice channel-picker visualization on the HA side. |
| Diagnose "why won't my network start" (RSSI/link/permit-join snapshot) | Skip | **v2.4 candidate** | Piggy-backs on energy scan |
| Radio TX power *read* (diagnostic) | Skip | **v2.4 candidate** | ZNP `SYS_GET_TX_POWER` on schedule. Setter stays with the client (see [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §3 sidebar). |
| WireGuard, DDNS | Skip | **Still skip** | Wrong layer regardless of v2 |
| Filesystem-over-HTTP | Skip | **Still skip** | Security anti-feature (see security-findings doc) |
| AI Assistant (Claude proxy) | Skip | **Still skip** | Security posture; not building agent-with-device-control |
| BE apps, Berry scripts, script integrations catalog | Skip | **Still skip** | Architectural — no on-device app runtime |
| Zigbee Hub (in-device Z2M-alike) | Skip | **Still skip** | Z2M/ZHA already do this in HA |
| USB gadget mode, CAN, IR, RF | Skip / per-device | **Still skip** | Hardware or per-device build, not v2 add-on scope |
| WiFi scan UI | Skip | **Still skip** | Native to HA's ESPHome integration |
| Runtime role/coord-mode switching | Skip | **Still skip** | Violates §24; compile-time only |
| Web auth, users, web UI | Skip | **Still skip** | Native ESPHome `web_server: auth:` |

#### 26.6.4 Scope-creep footgun

If v2 grows from "one-click flash" to "flash + backup + migrate + temp poll + energy scan", the add-on becomes A Whole Thing: HACS repo, release cadence, issue tracker, documentation site, breaking-change management. That's the actual cost, not the code.

Suggested phasing:

- **v2.0**: add-on ships with one-click flash + post-flash re-probe + progress events. Prove the architecture works.
- **v2.1**: ZB network backup export.
- **v2.2**: IEEE MAC read + migrate helper.
- **v2.3**: periodic radio SoC temp diagnostic.
- **v2.4**: energy scan + link diagnostics + TX power read.

Each increment reuses infrastructure the previous ones already built. Users who only want v1 keep paying nothing.

#### 26.6.5 The v1/v2 boundary in one line

**The boundary isn't "ESPHome-side vs HA-side." It's "things that only need the ESP32 (v1) vs things that need Python-side ZNP/Spinel talking to the radio (v2)."**

SLZB-OS collapses that boundary by putting a Berry runtime on the device. We keep the boundary and put Python on the HA side. That's the entire architectural disagreement between the two forks, distilled.

---

## 27. Feature parity and device-management surface

The SLZB-OS operator-facing feature set is retained by reusing SMLIGHT's own ESPHome packages (buzzer, IR, WS2812, BLE proxy, diagnostics, transport packages) and removing only the network-exposure and proprietary-UI ones. HA sees every retained feature as native ESPHome entities through the standard integration; no HA-side integration is required for v1. Post-v1 polish (a branded SLZB-MR4U device card) is a proposal to extend the existing `smlight` HA integration rather than a parallel new integration.

### 27.1 SMLIGHT HA integration — entity port/skip walkthrough

Cross-check of every entity the official [`smlight` HA integration](https://www.home-assistant.io/integrations/smlight/) exposes, against what we plan to expose from ESPHome via the Native API. **Port** = build our equivalent. **Skip** = intentionally not building it, with a reason.

| Category | Entity | Decision | Notes |
|---|---|---|---|
| sensor | `device_mode` | Port (v1) | Static text_sensor from hw_defs. Values: `LAN`, `LAN+Wi-Fi`, `USB`, etc. |
| sensor | `firmware_channel` per radio | Port (v1) | text_sensor from `firmware_channel` substitution (`prod` \| `dev`) |
| sensor | `zigbee_type` per radio | Port (v1) | text_sensor derived from `radio*_role` (`coordinator`/`router`/`RCP`/`NCP`/`primary controller`). Underlying two-dim `radio*_protocol` + `radio*_role` substitutions available separately as diagnostic sensors |
| sensor | core temperature | Port (v1) | ESPHome `internal_temperature:` component |
| number | core temperature offset | Port (v1) | SLZB-OS ships a `tempCalib.tempFx` offset (CMD 10 stores "here's the real temp"). Trivially replicated as a `number:` template calibration input feeding a `sensor:` `offset` filter. Nice UX for users with an accurate reference thermometer. |
| sensor | zigbee radio temperature | **Skip in v1** | SLZB-OS exposes it (`zb_temp`, `zb_temp2`) via a ZNP/Spinel MFG-INFO query, so it's obtainable — but reading it continuously would require breaking the `serial_proxy` dumb-pipe. Defer to v3+ pending a scheduled break-glass query pattern. |
| sensor | free RAM / FS / PSRAM | Port (v1) | ESPHome `debug:` component + `sensor:` |
| sensor | uptime | Port (v1) | ESPHome `uptime:` sensor |
| binary_sensor | ethernet | Port (v1) | ESPHome `ethernet.connected` binary_sensor |
| binary_sensor | wifi | Port (v1) | ESPHome `wifi.connected` binary_sensor |
| binary_sensor | vpn | **Skip** | No VPN client in this fork; would require WireGuard integration |
| binary_sensor | internet | Port (optional) | `http_request:` GET to a known endpoint every N minutes |
| switch | disable LEDs | Port (v1) | Global template switch driving all LED outputs |
| switch | night mode LEDs | Port (v1) | Scheduled variant of disable LEDs |
| switch | auto zigbee update | **Skip** | Update entity in HA already provides "Install" action; no automation loop needed |
| switch | VPN enabled | **Skip** | See VPN binary_sensor |
| button | core restart | Port (v1) | ESPHome `restart:` button |
| button | zigbee restart | Port (v1) | Pulse RST pin |
| button | zigbee flash mode | Port (v1) | DTR + RST bootloader-entry dance |
| button | reconnect Zigbee router | **Skip** | Router-mode-only feature; not exercised by primary user; add if MR4U user reports needing it |
| update | core firmware | **Skip in v1** | ESPHome already supports OTA; adding SMLIGHT-catalog-driven `update:` entity for the host firmware is redundant with existing ESPHome update flows |
| update | zigbee firmware per radio | Port (v1) | The whole point of this design. HA template `update:` entity backed by REST sensor |
| update | SSE flash progress stream | **Skip in v1** | Nice-to-have; falls out of v2 one-click flash design if we implement it |
| light | Ultima ambilight | N/A for MR4U | Ultima-only; add when Ultima gets its own local build |
| bluetooth | BLE proxy | Port (optional) | ESPHome `bluetooth_proxy:` component. Off by default in MR4U to keep image small |
| infrared | Ultima IR | N/A for MR4U | Ultima-only |
| service | `play_rtttl` on buzzer | Port (optional) | ESPHome `rtttl:` component if buzzer wired up (MRxU has one) |

**Summary**: v1 ports 13 entities directly, skips 6 for well-reasoned technical or scope reasons, defers 4 to per-device builds (Ultima) or later phases. No custom HA integration needed — the Native API surfaces all of these as native HA entities.

### 27.2 Retention via SMLIGHT ESPHome packages

SMLIGHT's `slzb-esphome` repo (which we fork) already contains ESPHome-native implementations of every feature the `smlight` integration currently wraps. By keeping SMLIGHT's packages and only removing the network-exposure ones, we retain the feature set at essentially zero extra cost:

| Feature (via `smlight` integration today) | Keep from `slzb-esphome` |
|---|---|
| Radio reset buttons | `packages/diagnostics/` |
| Buzzer + RTTTL playback | `packages/buzzer/` |
| IR TX / RX + code library | `packages/ir/`, `libraries/ir/codes/` |
| WS2812 LED effects + presets | `packages/ws2812/`, `packages/leds/` |
| BLE proxy | `packages/bluetooth/` (see §27.3) |
| Diagnostics sensors, firmware version | `packages/diagnostics/` |
| Firmware updates | ESPHome OTA |
| Wi-Fi transport | `packages/wifi/` |
| Ethernet transport | `packages/ethernet/` |

**Remove:** `packages/stream_servers/`, any web-UI packages, SLZB-OS-only server features.

### 27.3 Bluetooth proxy caveat

BLE proxy on the ESP32-S3 shares CPU and radio time with the UART transports. SMLIGHT's own README already warns that the on-board microphone "may cause instability or packet loss" on concurrent Zigbee/Thread/Z-Wave UART-to-Ethernet operation. The same concern applies to BLE proxy.

Note: the SLZB-OS BLE settings page itself links to "ESPHome BT proxy firmware" as the *alternative* to its built-in BLE feature — direct confirmation from SMLIGHT that ESPHome is the right stack for BLE on this hardware.

Rule for our firmware:

- **Retain** the BLE proxy capability in the YAML (from `packages/bluetooth/`).
- **Disable by default.** Ship v1 with BLE proxy off so it does not affect the primary Zigbee/Thread reliability testing.
- Expose it as an opt-in switch/config so users can enable and validate against their own workload.
- Do not enable BLE proxy during Phase 3 / Phase 5 soak tests — those must reflect the default configuration.

### 27.4 Post-v1 HA integration — extend, don't fork

**v1 — stock ESPHome integration, no HA-side code.**

ESPHome exposes entities transport-defined: any entity declared in the firmware YAML surfaces in HA automatically through the standard ESPHome integration. That means:

- BLE proxy is picked up by HA's Bluetooth integration as a proxy (first-class ESPHome feature).
- Buzzer RTTTL surfaces as `esphome.<device>_rtttl_input_set` — same call pattern SMLIGHT's README already documents.
- IR TX/RX, WS2812 effects, radio reset buttons, sensors — all appear as normal HA entities.

Functional coverage matches the SLZB-OS path. What is lost is polish: the device card shows "ESPHome mr4u" rather than a branded "SMLIGHT SLZB-MR4U" card, and there is no MR4U-specific setup wizard. SLZB-OS-only server features (SLZB-OS scripting, cloud, proprietary UI) are gone by design.

**Post-v1 — extend the existing `smlight` integration, do not fork.**

Same cooperation path as §26.3 Path (a). Propose adding an ESPHome-firmware backend to the existing `smlight` integration:

- Add a zeroconf matcher for `_esphomelib._tcp.local.` scoped to our device naming pattern (e.g. `slzb-mr4u-*`).
- On discovery, probe: SLZB-OS HTTP endpoint present? → use `pysmlight` backend. Not present? → adopt the device via the ESPHome integration and surface a curated subset of its entities in the `smlight` device card.
- Share the config flow, device registry entry, diagnostics, and update-entity plumbing across both backends.
- Auth: SLZB-OS backend uses its own token via `pysmlight`; ESPHome backend delegates to the ESPHome integration's PSK handling. No new secret storage.

This is a **larger** PR than the USB matcher work (touches coordinator, config flow, probably depends on `esphome` integration primitives), but it preserves the "SMLIGHT device" mental model regardless of which firmware the user runs. Discuss with `@tl-sl` before starting.

**Do not** write a standalone new integration. A parallel `smlight_esphome` domain duplicates the config flow, discovery, and coordinator layers and drifts out of sync over time.

### 27.5 Summary

- Feature parity: **retain** by reusing SMLIGHT's ESPHome packages.
- v1 HA integration: **none required** — stock ESPHome integration surfaces everything.
- Post-v1 polish: **extend the existing `smlight` integration**, don't fork it.
- BLE proxy: capability retained, **off by default**.

---

## 28. SLZB-OS features surveyed: disposition

Every operator-facing capability of the stock SLZB-OS web UI is either kept, retained-but-off (BLE proxy), or surfaced automatically through ESPHome. Features specific to SLZB-OS — on-device OTBR, Matter endpoint, Zigbee Hub, VPN, cloud firmware pull, raw-TCP USB passthrough, proprietary HTTP API, on-device scripting — are deliberately out of scope. Attack-surface reduction is the whole point of this fork.

### 28.1 SLZB-OS web UI — port/skip walkthrough

Cross-check against the 25 sections of the stock SLZB-OS web UI. Groups pages by whether their function is portable to our ESPHome + HA model.

| SLZB-OS section | Function | Decision | Notes |
|---|---|---|---|
| §1 Home / dashboard | Status overview | Port (v1) | HA dashboard replaces it; entities from §27 provide all data |
| §2 Mode (per-radio role picker) | Runtime role change | **Skip** | See [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §6. Compile-time only. `packages/roles/*.yaml` includes if we ever want a shortcut |
| §3 Zigbee settings | ZHA/Z2M host, port, channel | **Skip** | HA controls this via ZHA/Z2M config; not ESPHome's job |
| §4 Thread / OpenThread | OTBR admin | **Skip in v1** | Deferred; OT-RCP support falls out when we add `openthread:` component |
| §5 Ethernet settings | DHCP/static IP | Port (v1) | ESPHome `ethernet:` config in YAML; `use_address` substitution |
| §6 Wi-Fi settings | SSID/PSK | Port (v1) | ESPHome `wifi:` config; secrets |
| §7 VPN | WireGuard client | **Skip** | Out of scope for this fork |
| §8 System / hostname | Rename device | Port (v1) | ESPHome `name:` + friendly_name |
| §9 System / timezone | NTP + TZ | Port (v1) | ESPHome `time:` component |
| §10 System / restart | Reboot host | Port (v1) | ESPHome `restart:` button |
| §11 System / reset config | Factory reset | Port (optional) | ESPHome `factory_reset:` button; behind a confirmation |
| §12 Users / password | Web-UI auth | **Skip** | We don't ship a web UI. Native API handles auth via encryption key |
| §13 LED settings | Brightness/mode | Port (v1) | Existing led packages + disable/night switches |
| §14 Backup / restore | Config backup | **Skip** | HA snapshots handle this; not our layer |
| §15 SSH | Enable SSH | **Skip** | Not applicable |
| §16 Logs (host) | View logs | Port (v1) | ESPHome logger + HA logbook |
| §17 Logs (radio) | View radio serial | Port (optional) | ESPHome `uart:` `debug:` block; noisy, off by default |
| §18 Update host firmware | Check + install | Port (partial) | ESPHome OTA already handles install; catalog check for host is skipped (see §27) |
| §19 Update Zigpy-znp / ZHA | Radio protocol lib | **Skip** | HA-side; not ESPHome's concern |
| §20 Update SLZB-OS | Host OS | **Skip** | Replaced by ESPHome OTA |
| §21 Firmware update (radio) | Check + flash per radio | Port (v1 check, v2 flash) | The central feature of this design |
| §22 Buzzer test | Play tone | Port (optional) | ESPHome `rtttl:` action button |
| §23 Buttons test | Hardware self-test | **Skip** | One-off diagnostic; not worth automating |
| §24 About | Device info | Port (v1) | ESPHome diagnostic sensors already publish this |
| §25 API reference | Docs | **Skip** | ESPHome Native API is a different API; docs live in this repo |

**Summary**: 14 sections port cleanly, 11 skip for reasons that boil down to "wrong layer for this fork" (auth, VPN, backup, host-OS updates) or "compile-time-only" (role change). The port/skip split validates that the ESPHome + HA-Native-API model covers the useful surface without needing a custom integration.

### 28.2 Feature disposition — keep vs skip vs retain-off

A sweep of the stock SLZB-OS web UI on the MR4U surfaces several features beyond Zigbee/Thread transport. Each is dispositioned explicitly so future contributors know what was considered and why it isn't in v1.

| SLZB-OS feature | v1 disposition | Rationale |
|---|---|---|
| Zigbee coordinator on either radio | Keep | Primary use case; either UART can carry it. |
| Thread to remote OTBR | Keep | Our primary Thread path (see [integration-recipes.md](../integration-recipes.md) §3). |
| Thread + on-device OTBR (beta) | Non-goal | Border-router state/routing belong in a supervised add-on, not on the MCU. |
| Matter-over-Thread mode on the gateway | Non-goal | HA already brokers Matter via OTBR. |
| Zigbee Hub (beta) | Non-goal | We host no Zigbee application layer on-device. |
| USB-to-Ethernet passthrough (SLZB-OS :8638 for external USB dongles) | Non-goal | Same insecure raw-TCP category as :6638/:7638. If needed later, expose via `serial_proxy` over encrypted Native API. |
| VPN | Non-goal | Network reachability is the operator's problem; no cloud/proprietary dependency. |
| Cloud firmware-update check | Non-goal | Manual, encrypted radio-firmware flow is future work (see [roadmap.md](roadmap.md) v2). |
| BLE + BLE proxy | Retain, off by default (§27.3) | SLZB-OS itself links to "ESPHome BT proxy firmware" as the alternative. |
| BLE scan interval / window tuning | Expose as ESPHome numbers | Trivial from `packages/bluetooth/`. |
| Radio reset / bootloader entry buttons | Keep | Native API actions, authenticated. See §15. |
| IEEE address read/write | Keep | Essential for coordinator migration without re-pairing. Ownership rule applies. See §15, §29. |
| Zigbee channel energy scan | Keep | Clean-channel diagnostic. Ownership rule applies. |
| Buzzer + RTTTL, IR TX/RX, WS2812 effects | Keep (§27.2) | Reused SMLIGHT ESPHome packages. |
| Dashboard: SoC temperature, uptime, radio FW versions, connection status | Expose as ESPHome sensors | Free via ESPHome; surfaces to HA automatically. |
| Concurrent USB + Wi-Fi/Ethernet + web server | Not exposed | Hardware supports it; firmware keeps transports mutex at build time for UX/testing simplicity. See §25. |
| SLZB-OS web UI, scripting, proprietary HTTP API | Removed | Attack-surface reduction is the whole point of this project. See §3. |
| ADVANCED socket options (Zigbee Socket packet processing, multi-threaded socket, Multi-Radio Queue Control) | Not applicable | These are workarounds for raw-TCP-socket semantics, which we don't expose. |

Bold summary: every operator-facing capability is either **kept**, **retained-but-off**, or **surfaced automatically** through ESPHome. Everything SLZB-OS-specific (on-device OTBR, Matter endpoint, Zigbee Hub, VPN, cloud FW pull, raw-TCP USB passthrough, proprietary UI) is deliberately out and stays out.

---

## 29. Migration from SLZB-OS

Practical delta for existing SLZB-OS users switching to our firmware. This section is written for the operator, not the implementer.

### 29.1 Coordinator URL translation

The user-visible config that changes most is the ZHA/Z2M coordinator URL. Port names below (`zigbee`, `thread`, `zwave`) are the `serial_proxy` `name:` values from the firmware YAML (§8) and are stable across releases.

| Scenario | Today (SLZB-OS raw TCP) | Our firmware (`ethernet` / `wifi` build) | Our firmware (`usb` build) |
|---|---|---|---|
| ZHA on EFR32 (EmberZNet) | Radio type `EZSP`, `socket://<mr4u-ip>:6638` | Non-default MR4U build — reflash EFR32 with EmberZNet and edit the device YAML so the EFR32's `serial_proxy` `name:` reads `zigbee` (or similar). ZHA then targets `esphome-hass://<config_entry_id>?port_name=<that-name>` | `/dev/serial/by-id/usb-...` (auto-discovered, see §25), radio type `ezsp` |
| ZHA on CC2674 (Z-Stack) — **default MR4U** | Radio type `znp`, `socket://<mr4u-ip>:7638` | Add ESPHome device (host + PSK) in HA; ZHA picks `esphome-hass://<config_entry_id>?port_name=zigbee` | `/dev/serial/by-id/usb-...`, radio type `znp` |
| Z2M (either radio) | `port: tcp://<mr4u-ip>:<6638\|7638>` | `port: esphome://<mr4u-ip>:6053/?port_name=<zigbee\|thread>` (Z2M consumes serialx when configured to), PSK in Z2M config | `port: /dev/serial/by-id/usb-...`, `adapter: ezsp` (EFR32) or `adapter: zstack` (CC2674) |
| OTBR add-on | Radio URL points at SLZB-OS TCP port | `radio: type: esphome, host: <mr4u-ip>, psk: ..., port_name: thread` (per §11.2) | OTBR consumes the CDC device path exposed by the HA hardware wrapper (§25.4) |
| BLE proxy | SLZB-OS BLE feature | Package retained but **off by default** — opt in by uncommenting `platform_ble` in the device YAML (tame passive-scan defaults; SLZB-OS itself recommends ESPHome BLE proxy — see §27.3, §30.4) | n/a (no networking) |

ZHA and OTBR must target **different** chips — §14 forbids sharing a single `serial_proxy` port between clients.

### 29.2 Preserving Zigbee pairings

Re-pairing every Zigbee device is painful. Correct sequence:

1. On SLZB-OS, read the current coordinator IEEE address (Advanced → Adapter IEEE address change → Read current IEEE). Record it.
2. Reflash to our firmware.
3. Via the authenticated management action (`zigbee_ieee_write`, §15), write the same IEEE onto the same radio.
4. Stop ZHA/Z2M, power-cycle Zigbee routers (≥15s), power them back on, then re-enable ZHA/Z2M with the new URL from §29.1.

Devices should reconnect within 5–10 minutes.

### 29.3 What is intentionally lost

- SLZB-OS web UI, scripting, cloud firmware-check, VPN — all removed by design (§3, §18, §28).
- "Device card" branding as "SMLIGHT SLZB-MR4U" in HA — v1 shows "ESPHome mr4u". Post-v1 polish path is §27.2 (extend the existing `smlight` HA integration).
- On-device OTBR / Matter endpoint / Zigbee Hub — explicit non-goals (§18). Users who need on-device OTBR should stay on SLZB-OS.

---

## 30. Delta from upstream

Reference inventory of what changed relative to [`smlight-tech/slzb-esphome`](https://github.com/smlight-tech/slzb-esphome), the ESPHome build this fork descends from. Complements §7 (transport), §17 (hardening), and §18 (non-goals), which explain *why*. This section is the compact *what* the top-level README points at instead of duplicating the full delta on the front page.

### 30.1 Transport / auth / OTA / radio-firmware-reporting delta

| Concern | Upstream | This fork |
|---|---|---|
| Radio UART transport | Plaintext TCP (`stream_server`) | Encrypted [ESPHome Native API](https://esphome.io/components/api.html) via [`serial_proxy`](https://esphome.io/components/serial_proxy.html) |
| HA-side URL | `socket://<ip>:<port>` | `esphome-hass://esphome/{entry_id}?port_name=<zigbee\|thread\|zwave>` |
| Auth | None (open TCP) | Pre-shared `device_encryption_key` (Noise `NNpsk0` + ChaCha20-Poly1305) |
| Radio reset / bootloader entry | HA switches writing GPIO | Automatic — `serial_proxy` drives `dtr_pin` (nRESET) and `rts_pin` (bootloader) from the client's DTR/RTS modem-control signals (matches `zigpy-znp`, `universal-silabs-flasher`, `bellows`, `zwave-js`) |
| OTA | Unauthenticated | Encrypted with the same PSK (`ota: encryption:` inherits the API key) — requires ESPHome 2026.9+ |
| USB pass-through (`packages/usb/usb_uart.yaml`) | Plaintext TCP `:9638` | `serial_proxy` (port name `usb`) — package swaps the transport; opt-in for SLWF-09U via a commented `!include` in [`devices/slw09u_r1_01.yaml`](../../devices/slw09u_r1_01.yaml). Not shipped by default. (§25 covers a separate future USB-**device**-mode CDC bridge — a different animal from this USB-host pass-through.) |
| Radio firmware version reporting | Not exposed to HA | One-shot boot-time probe per radio (ZNP `SYS_VERSION` for CC26xx and Spinel `PROP_NCP_VERSION` for EFR32 Thread; EZSP and Z-Wave still publish `"unknown (<protocol> probe not implemented in v1)"` until real probes ship in a later v1.x release); published as diagnostic sensors; HA template snippet included for update-available comparison against SMLIGHT's public catalog (see [`docs/ha-integrations/`](../ha-integrations/)) |

### 30.2 What is preserved from upstream

- All hardware definitions, HAL packages, LEDs / buttons / buzzer / IR / WS2812 logic (basic effects and lambda effects; no music-reactive effects — see §30.3).
- All supported devices (ULTIMA, MRxU, 06xU, SLWF-09U) and the device-composition-driven build model (§8).

### 30.3 What is removed

- `packages/stream_servers/` (whole directory) — obsolete under §7.
- `packages/external_components/stream_server.yaml` — `serial_proxy` is a first-class ESPHome component, no external source needed.
- `packages/buses/uarts/uart_ctrl/` (whole directory) — the per-radio `RST` / `FLASH` GPIO-switch wrappers. Now performed automatically by `serial_proxy` on behalf of the connected client (§15).
- `components/music_leds/`, `components/fastled_helper/`, `packages/buses/i2s_mic.yaml`, `packages/ws2812/effects/music_leds.yaml` — the WLED-derived sound-reactive WS2812 effects and their I2S-microphone driver, along with the FastLED library dependency they pulled in. Full rationale in §18 (radio-transport reliability pillar).

### 30.4 What is off by default (opt-in)

Anything on the SoC that meaningfully competes with the radio UART loops for CPU, interrupt budget or shared RF frontend time is a liability for the fork's primary job. Where the upstream tree defaulted to "on and aggressive", this fork defaults to "off, opt-in with tame settings":

- **Bluetooth proxy** (`packages/bluetooth/bluetooth.yaml`). Package kept; not `!include`d by any shipping device build in v1. Upstream defaults were 100% active BLE scanning (`interval == window == 1100ms`, `active: true`) plus `bluetooth_proxy: active: true`. Full rationale in §18 and §27.3. Opt-in defaults now ship as passive scan, ~10% duty cycle, `bluetooth_proxy: active: false`.
- **`logger:` verbosity** (`packages/core/core.yaml`). Pinned to `INFO`. ESPHome's default is `DEBUG`, which at the loop level competes with `serial_proxy`'s per-byte servicing. Override in a device file for a dev build.
- **`ir_codes_tv_lg` on Ultima** (`libraries/ir/codes/tv_lg.yaml`). Commented out in `devices/ultima_r1_04.yaml`. IR code packs are content, not infrastructure — shipping a specific vendor pack by default was arbitrary. Uncomment in your device YAML to opt in.

### 30.5 What you will miss compared to upstream

Summary; see §18 for the full non-goal register with rationale:

- **Sound-reactive WS2812 effects driven by the on-board microphone** (upstream's third-party `music_leds` / `fastled_helper` — 16 named effects, FFT + AGC + peak-detect pipeline). Rationale: §18.
- **The on-board I2S microphone as an exposed HA entity** (ICS-41414 on SLZB-Ultima and SLWF-09U). The hardware is still on the board, but no ESPHome package in this fork claims it.
- **The FastLED Arduino library** as a build dependency. Nothing else in the tree used it; removing `music_leds` removed the last caller. Result: smaller image, faster compile, one less external library to break on toolchain updates.

### 30.6 HA-side entity changes

Per-radio, the following Home Assistant entities are **no longer created**:

- `<friendly> <radio> RST` switch
- `<friendly> <radio> FLASH` switch
- `<friendly> <radio> TCP Connected` binary_sensor

Manual radio reset from the HA dashboard is not required in normal operation — the flasher / integration handles DTR/RTS itself over the encrypted Native API (§15).

