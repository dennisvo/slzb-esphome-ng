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
TCP 6053   ESPHome Native API   (Noise + PSK)
TCP 6638   CLOSED
TCP 7638   CLOSED
TCP 8638   CLOSED
TCP 80     CLOSED
```

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

See [integration-recipes.md](integration-recipes.md) §1 for both URL schemes (`esphome-hass://` inside HA Core, `esphome://` from separate containers) and their operational consequences.

---

## 7. ESPHome Native API encryption

ESPHome's Native API supports Noise `NNpsk0` with ChaCha20-Poly1305 AEAD. Message type, length and protobuf payload are all inside the authenticated encrypted envelope.

```yaml
api:
  encryption:
    key: !secret mr4u_api_key
```

The PSK is a device credential and MUST be treated as such. OTA uses a separate credential:

```yaml
ota:
  - platform: esphome
    password: !secret ota_password
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
| 2 | [`home-assistant/core`](https://github.com/home-assistant/core) | ZHA, ESPHome integration, `serialx` registration. | **Ideally no changes.** Only patch if §10 validation shows a layer bypassing `serialx` that cannot be fixed in `zigpy` or a radio backend. |
| 3 | [`home-assistant/addons`](https://github.com/home-assistant/addons) | The OpenThread Border Router add-on. | **Prototype the `serialx esphome:// → PTY → otbr-agent` adapter here** (§11). |

Development ordering:

```text
smlight-tech/slzb-esphome        ← START HERE
        │
        └── our fork
             └── secure MR4U firmware  (Phases 1–3)

home-assistant/core              ← test ZHA against firmware; patch only if required (Phase 2)

home-assistant/addons            ← OTBR ESPHome-serial adapter prototype (Phase 4)
```

Get the MR4U booting with two encrypted `serial_proxy` instances and passing `nmap` (Phase 1) before touching either Home Assistant repository.

### Illustrative YAML (exact pins from upstream hardware definitions)

```yaml
api:
  encryption:
    key: !secret api_encryption_key

ota:
  - platform: esphome
    password: !secret ota_password

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
  - id: efr32
    uart_id: efr32_uart
    name: "EFR32 radio"     # Thread by default (SMLIGHT MR4U default)

  - id: cc2674
    uart_id: cc2674_uart
    name: "CC2674 radio"    # Zigbee by default
```

Explicitly absent: `stream_server`, `web_server`, `mqtt`.

**Port name convention.** The `port_name=<x>` in the `serialx` URL matches the `id` of the corresponding `serial_proxy`. This design names ports by **chip** (`efr32`, `cc2674`), mirroring SLZB-OS's hardware-based port numbering (6638 = EFR32, 7638 = CC2674) — which reflects the truth that either chip can play either role. The operator points ZHA at whichever chip they want as the Zigbee coordinator, and OTBR at the other. Default mapping in a fresh build (matches SMLIGHT's factory config): `cc2674` = Zigbee, `efr32` = Thread. See §29.1 for URL examples in both configurations.

---

## 9. Zigbee integration (ZHA)

ZHA is the v1 primary integration target. The transport chain is: ZHA → zigpy → protocol-specific radio backend (`zigpy-znp` for CC2674, `bellows` for EFR32) → `zigpy.serial` → `serialx` → `esphome-hass://` → `serial_proxy` on the device. No component in that chain is host-side compatibility glue — every layer is stock upstream.

See [integration-recipes.md](integration-recipes.md) §2 for the ZHA pairing flow, the ZNP/EZSP wire-protocol context, Z2M as a future client (paths A and B), and the anti-features list.

---

## 10. ZHA validation (must be done, not assumed)

Not every integration reliably reaches `serial_proxy` through `serialx`; some historically call pyserial directly. Verify the whole chain:

1. Does ZHA open the coordinator via `serialx` for the selected zigpy backend?
2. Does the CC2674-compatible zigpy library propagate the transport (no direct pyserial call)?
3. Can `esphome-hass://…` be opened without URL rewriting?
4. Are baudrate and flow-control requests propagated correctly?
5. What happens when the underlying ESPHome API session reconnects?

If any layer bypasses `serialx`, the preferred fix is upstream (HA, zigpy, or the zigpy radio backend) — not a compatibility proxy on the host.

---

## 11. Thread integration (OTBR)

`otbr-agent` only accepts `spinel+hdlc+uart://<posix-path>` URLs — it does not speak `serialx` natively. Our path is a small Python adapter, running inside the HA OTBR add-on's own container, that opens `esphome-hass://` via `serialx`, creates a PTY via `os.openpty()`, symlinks it to `/tmp/ttyOTBR`, and forwards bytes bidirectionally. `otbr-agent` sees a normal POSIX serial device; the encryption and transport are transparent to it.

See [integration-recipes.md](integration-recipes.md) §3 for the Spinel primer, adapter architecture, add-on config surface, supervised-failure lifecycle, and Thread/Matter commissioning flow.

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

`serial_proxy` accepts a `flow_control` parameter and exposes `rts_pin`/`dtr_pin` as modem-control outputs, but transparent RTS/CTS across ESPHome/ESP-IDF is not currently plumbed by the upstream `uart:` component. Phase 1 ships `components/uart_hw_flow/` — a ~20 LOC fork-local external component that calls ESP-IDF `uart_set_hw_flow_ctrl(UART_HW_FLOWCTRL_CTS_RTS, …)` on the configured UART, controlled per-UART via the `uart*_hw_flow: true|false` substitution in `hw_defs/**`. Current MR4U radios keep it `false` (matches SMLIGHT firmware defaults); the axis exists so future radios that require HW FC (e.g. MG24 Thread) work by config alone. An upstream ESPHome PR adding `cts_pin`/`rts_pin` to `uart:` remains the preferred long-term landing; the external component is the interim carrier. See [v1-design.md](v1-design.md) §5.

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
- Bluetooth proxy **enabled by default** (capability retained in firmware but disabled — see [v1-design.md §11.3](v1-design.md))
- Automatic radio firmware flashing
- Cloud dependency (including SLZB-OS-style cloud firmware-update checks and VPN)
- Multi-client sharing of a coordinator
- Modifying OpenThread itself to speak `esphome://`
- **On-device OTBR** (SLZB-OS "Thread+OTBR running on device (beta)") — border-router responsibilities stay in a supervised HA add-on
- **Matter-over-Thread endpoint mode** on the gateway itself — HA brokers Matter via OTBR
- **Zigbee Hub** / any on-device Zigbee application layer
- **USB-to-Ethernet passthrough** for external USB dongles plugged into the MR4U's USB host port (SLZB-OS TCP :8638) — same insecure category as :6638/:7638; if wanted later, expose via `serial_proxy` on the encrypted Native API, not raw TCP
- **On-board microphone as an HA entity, and sound-reactive WS2812 effects (`music_leds` + `fastled_helper`)** — upstream ships these via the third-party [`andrewjswan/esphome-components`](https://github.com/andrewjswan/esphome-components) source tree. Removed from this fork on 2026-09-10 along with the FastLED library dependency. The FFT + AGC + peak-detection task on the second core + WS2812 RMT output competes with `serial_proxy`'s radio UART loops for CPU / interrupt budget, producing intermittent byte drops on 115200–460800-baud Zigbee / Thread / Z-Wave streams. Attack-surface reduction is the fork's headline motive; **radio-transport reliability** is its second, non-negotiable pillar — anything on the SoC that jeopardizes it fails the ship criterion by construction. If sound-reactive effects ever return, they will be gated behind an explicit runtime "pause radios while mic active" switch, not on-by-default.

---

## 19. Phased implementation plan

The Phases below are the architectural milestones this document builds toward. [roadmap.md](roadmap.md) is organized by shipped user-facing version (v1, v1.x, v2) rather than by Phase, since one Phase can span multiple releases and one release can advance multiple Phases. The two views are complementary; roadmap.md is the authoritative source for what ships when.

### Status snapshot

| Phase | State | Notes |
|---|---|---|
| Phase 0 — Upstream inspection | ✅ Done | commit `7c507be`; ground-truth references in [`docs/research/`](research/) |
| Phase 1 — Secure firmware (repo-wide) | ✅ Done | commits `307fd30`, `3a0ebef` — pushed to `origin/secure-native-api`. Empirical hardware verification pending (rolled into Phase 2 acceptance). |
| Phase 2 — Native ZHA validation | ⏳ Pending | Requires hardware in hand |
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

**Aspirational end-state: zero HA-side glue.** The "finished" version of this design has *zero* patches or adapters outside our own ESPHome firmware fork. Two glue points remain in v1, and both have concrete upstream retirement paths:

| Glue in v1 | What retires it | Section |
|---|---|---|
| OTBR add-on internal PTY adapter | Upstream `spinel+hdlc+esphome://` scheme in OpenThread | §11.4 |
| Possible zigpy radio-backend shim (if §10 validation finds `pyserial` bypass) | Upstream fix in the affected zigpy backend to use `serialx` | §10 |

Both are the same insight applied to the two radios. The day this design ships with zero HA-side patches — stock ESPHome integration, stock ZHA, stock OTBR add-on, stock Thread integration — is the day it is finished. Everything until then is bridging code we are actively trying to delete.

**Push-further point 1: land `spinel+hdlc+esphome://` upstream (see §11.4).**

OpenThread's radio-URL syntax is pluggable — schemes like `spinel+hdlc+uart:///dev/ttyUSB0` and `spinel+hdlc+forkpty:///path/to/program` are each a small C++ class in `openthread/src/posix/platform/`. Adding an `esphome://` scheme means writing one such class that:

1. Negotiates a Noise session against ESPHome's Native API on open
2. Subscribes to the `serial_proxy` for the configured `port_name`
3. Feeds RX bytes into HDLC framing and pumps TX bytes out
4. Handles reconnect

With this in place, `otbr-agent` config becomes literally one line: `RADIO_URL=spinel+hdlc+esphome://mr4u:6053/?port_name=efr32`. The §11 PTY adapter, its supervision logic, and its config surface all vanish. Every other user of a remote encrypted RCP benefits too, so it's a real upstream contribution, not a hostile patch we push for our own use. Non-trivial C++ in a security-sensitive codebase and OpenThread's release cadence is slow — hence "future," hence the pragmatic PTY bridge in v1.

**Push-further point 2: push §10 validation to completion early.**

The ZHA serial-open path is `ZHA → zigpy → radio backend → serialx → transport handler → wire`. In theory `serialx` transparently handles `esphome-hass://`. In practice some radio backends historically bypassed `serialx` and called `pyserial` directly. If any layer in that chain does so, `esphome-hass://` fails with a URL-scheme error.

Why it must be done early:

1. *If we discover this late, someone will suggest a compatibility shim* — a local `pyserial`-mimicking process that proxies to `esphome://`. That's exactly the "unnecessary plugin" we're trying to avoid; once shipped, it never dies, and the design's minimalism narrative collapses.
2. *Landing a zigpy fix now* means a clean HA release picks it up naturally. Landing it after users are relying on a workaround means coordinating a migration.

The validation is small — open ZHA against `esphome-hass://.../cc2674`, verify `serialx` is on the call stack when the transport opens, and if not, walk the stack to find who's calling `pyserial.Serial(...)` directly. Then land a PR against that library to route through `serialx`.

**Both points converge on the same north star.** Every shim retired is a plugin we said we didn't want and now don't have. That is a healthier trajectory than most integration projects, which tend to accumulate glue over time rather than shed it.

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

---

## 27. Feature parity and device-management surface

The SLZB-OS operator-facing feature set is retained by reusing SMLIGHT's own ESPHome packages (buzzer, IR, WS2812, BLE proxy, diagnostics, transport packages) and removing only the network-exposure and proprietary-UI ones. HA sees every retained feature as native ESPHome entities through the standard integration; no HA-side integration is required for v1. Post-v1 polish (a branded SLZB-MR4U device card) is a proposal to extend the existing `smlight` HA integration rather than a parallel new integration.

See [v1-design.md](v1-design.md) §11 for the per-feature retention table, the two-stage HA integration strategy, and the BLE-proxy-off-by-default rule.

---

## 28. SLZB-OS features surveyed: disposition

Every operator-facing capability of the stock SLZB-OS web UI is either kept, retained-but-off (BLE proxy), or surfaced automatically through ESPHome. Features specific to SLZB-OS — on-device OTBR, Matter endpoint, Zigbee Hub, VPN, cloud firmware pull, raw-TCP USB passthrough, proprietary HTTP API, on-device scripting — are deliberately out of scope. Attack-surface reduction is the whole point of this fork.

See [v1-design.md](v1-design.md) §12 for the per-feature disposition table.

---

## 29. Migration from SLZB-OS

Practical delta for existing SLZB-OS users switching to our firmware. This section is written for the operator, not the implementer.

### 29.1 Coordinator URL translation

The user-visible config that changes most is the ZHA/Z2M coordinator URL. Port names below (`efr32`, `cc2674`) are the `serial_proxy` IDs from the firmware YAML (§8) and are stable across releases.

| Scenario | Today (SLZB-OS raw TCP) | Our firmware (`ethernet` / `wifi` build) | Our firmware (`usb` build) |
|---|---|---|---|
| ZHA on EFR32 (EmberZNet) | Radio type `EZSP`, `socket://<mr4u-ip>:6638` | Add ESPHome device (host + PSK) in HA; ZHA picks `esphome-hass://<config_entry_id>?port_name=efr32` | `/dev/serial/by-id/usb-...` (auto-discovered, see §25), radio type `ezsp` |
| ZHA on CC2674 (Z-Stack) | Radio type `znp`, `socket://<mr4u-ip>:7638` | Same flow, `port_name=cc2674` | `/dev/serial/by-id/usb-...`, radio type `znp` |
| Z2M (either radio) | `port: tcp://<mr4u-ip>:<6638\|7638>` | `port: esphome://<mr4u-ip>:6053/?port_name=<efr32\|cc2674>` (Z2M consumes serialx when configured to), PSK in Z2M config | `port: /dev/serial/by-id/usb-...`, `adapter: ezsp` (EFR32) or `adapter: zstack` (CC2674) |
| OTBR add-on | Radio URL points at SLZB-OS TCP port | `radio: type: esphome, host: <mr4u-ip>, psk: ..., port_name: <efr32\|cc2674>` (per §11.2 — whichever chip runs Thread) | OTBR consumes the CDC device path exposed by the HA hardware wrapper (§25.4) |
| BLE proxy | SLZB-OS BLE feature | Package retained but **off by default** — opt in by uncommenting `platform_ble` in the device YAML (tame passive-scan defaults; SLZB-OS itself recommends ESPHome BLE proxy — see [v1-design.md §11.3](v1-design.md), §30.4) | n/a (no networking) |

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
| Auth | None (open TCP) | Pre-shared `api_encryption_key` (Noise `NNpsk0` + ChaCha20-Poly1305) |
| Radio reset / bootloader entry | HA switches writing GPIO | Automatic — `serial_proxy` drives `dtr_pin` (nRESET) and `rts_pin` (bootloader) from the client's DTR/RTS modem-control signals (matches `zigpy-znp`, `universal-silabs-flasher`, `bellows`, `zwave-js`) |
| OTA | Unauthenticated | Password-protected (`ota_password`) |
| USB pass-through (`packages/usb/usb_uart.yaml`) | Plaintext TCP `:9638` | `serial_proxy` (port name `usb`) — package exists and swaps the transport, but is not `!include`d by any shipping device build in v1 (kept ready for a future USB-host variant per §25) |
| Radio firmware version reporting | Not exposed to HA | One-shot boot-time probe per radio (ZNP `SYS_VERSION` for CC26xx in v1; Spinel, EZSP and Z-Wave stubs publish `"unknown (<protocol> probe not implemented in v1)"` until real probes ship in v1.x); published as diagnostic sensors; HA template snippet included for update-available comparison against SMLIGHT's public catalog (see [`docs/ha-integrations/`](ha-integrations/)) |

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

- **Bluetooth proxy** (`packages/bluetooth/bluetooth.yaml`). Package kept; not `!include`d by any shipping device build in v1. Upstream defaults were 100% active BLE scanning (`interval == window == 1100ms`, `active: true`) plus `bluetooth_proxy: active: true`. Full rationale in §18 and [v1-design.md §11.3](v1-design.md). Opt-in defaults now ship as passive scan, ~10% duty cycle, `bluetooth_proxy: active: false`.
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

