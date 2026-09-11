# Roadmap — slzb-esphome-ng

Living plan for what ships in each release, what's deferred, and what we've
explicitly decided not to do. Update this in the same PR as the code that
moves an item between sections.

Legend: 🟢 shipped · 🟡 in flight · ⚪ planned · 🔵 stretch · ❌ won't do

Cross-references:
- [design.md](design.md) — architecture and phase model
- [v1-radio-firmware.md](v1-radio-firmware.md) — radio version visibility (v1 authoritative design)
- [architecture.md](architecture.md) — repo layout and package model

---

## v1 — probe + HA update notifications 🟡

Goal: users can see per-radio installed firmware in HA and get a native "update
available" card comparing against the SMLIGHT catalog. Zero flashing from us.
All radio-identity metadata is compile-time substitution — no runtime knobs.

**Design authority**: [v1-radio-firmware.md](v1-radio-firmware.md) §§3–7, §10.

**Status (2026-09-10)**: code + configuration merged (see commit graph); pending
hardware validation on MR4U. Flip 🟡 → 🟢 once the log line + HA sensor value
have been confirmed against SLZB-OS's own reading.

### Step 1 — ZNP probe (MR4U Radio 1) 🟢

Prove the whole pattern on one radio, one protocol, one device.

- 🟢 `components/radio_probe/__init__.py` — codegen + cross-field validator enforcing §6g permutation matrix
- 🟢 `components/radio_probe/radio_probe.{h,cpp}` — component skeleton, `setup_priority = 250`, dispatcher on `protocol` enum
- 🟢 `components/radio_probe/znp_probe.cpp` — `SYS_VERSION` (0x2102), parse `TransportRev/Product/Major/Minor/Maint/Rev(LE u32)`, format `Rev` as YYYYMMDD
- 🟢 `packages/diagnostics/radio_probe_ext.yaml` + `radioN_firmware_info.yaml` — template sensors from substitutions + `radio_probe:` instance publishing `installed_firmware` (split per-radio to match existing per-UART package pattern; UART id parametrized via `radioN_uart_id` substitution so single-radio boards like 06xU on UART2 still surface as "Radio 1")
- 🟢 `hw_defs/mrxu/mr4u_r1_73.yaml` — add `radio1_*` substitutions (chip, smlight_id, protocol=`znp`, role=`coord`, firmware_channel=`dev`)
- 🟢 `devices/mr4u_r1_73.yaml` — include diagnostics package
- 🟢 `docs/radio-firmware/catalog-snapshot.json` — committed snapshot of the catalog at a known date, for matrix-drift review
- 🟢 `docs/radio-firmware/refresh.py` — ~30 LOC fetcher; run manually, review git diff before commit
- 🟢 `docs/radio-firmware/README.md` — what/why/refresh workflow

Success: MR4U flashed, ESPHome log shows `[radio_probe] Radio 1: rev = 20260311`, HA sensor matches SLZB-OS's own dialog value.

### Step 2 — Spinel + EZSP + Z-Wave stubs 🟢

Publish `"unknown (<protocol> probe not implemented in v1)"` for radios we
can't yet decode. Prefer honest ignorance to silent no-value sensors. This
covers MR4U Radio 2 (EFR32MG26 Thread), any EFR32 coord builds, and the
Ultima ZW-800.

- 🟢 Stub dispatch lives inline in `components/radio_probe/radio_probe.cpp` (single-file dispatcher covers all three branches; no separate `stub_probe.cpp` needed — collapsed at implementation time)
- 🟢 `hw_defs/mrxu/mr4u_r1_73.yaml` — add `radio2_*` substitutions (protocol=`spinel`, role=`rcp`)
- 🟢 Spinel stub replaced by a live probe post-v1 — see v1.x below
- ⚪ EZSP + Z-Wave stubs still to replace — see v1.x below

### Step 3 — extend to other devices 🟢

- 🟢 `hw_defs/ultima/r1_04.yaml` — 3 radios (CC26 coord, EFR32 coord-or-thread, ZW-800)
- 🟢 `hw_defs/06xu/r1_73.yaml` — 1 generic radio slot (default CC2674P10 ZNP coord; users of EFR32 variants override)
- 🟢 `hw_defs/slw09u/r1_01.yaml` — no `radioN_*` (no radio); marker comment lives in device YAML instead
- 🟢 `devices/ultima_r1_04.yaml`, `devices/06xu_r1_73.yaml` — include diagnostics package
- 🟢 `devices/slw09u_r1_01.yaml` — deliberately does not include (comment in file)

### Step 4 — HA snippet + docs 🟢

- 🟢 `docs/ha-integrations/smlight-firmware-update.yaml` — HA `rest:` catalog fetcher + per-radio `template: update:` entity with `availability:` guard on `"unknown …"`; filters catalog by chip → SMLIGHT type (from role) → baud → channel
- 🟢 `docs/ha-integrations/README.md` — install steps, per-radio v1 coverage table, recorder-exclusion guidance, re-flash-out-of-band note (live probe re-reads on boot; no YAML edit needed)
- 🟢 Top-level [README.md](../README.md) "Radio firmware version visibility in HA" row now points at `docs/ha-integrations/` instead of the design draft; radio-firmware feature bullet and comparison table both scoped to "ZNP live in v1, other protocols stubbed" so nothing overpromises

### Step 5 — UART hardware flow control component 🟢

Upstream ESPHome's `uart:` platform has no YAML surface for classic RTS/CTS handshake (`flow_control_pin` is for RS485 driver-enable only). Without this component the `hwFlow` filter axis in the HA update catalog Jinja is a lie — we'd offer firmware images the ESP32 side can't actually honor. Ships as v1 for correctness-by-construction, even though every stock SMLIGHT catalog entry currently visible to MR4U has `hwFlow` absent/false.

**Design authority**: [v1-design.md](v1-design.md) §5.

- 🟢 `components/uart_hw_flow/{__init__.py, uart_hw_flow.h, uart_hw_flow.cpp}` — `Component` at `setup_priority 250`; static_casts parent to `uart::IDFUARTComponent`, calls `uart_set_pin()` + `uart_set_hw_flow_ctrl(port, UART_HW_FLOWCTRL_CTS_RTS, 122)` on `setup()`; `enabled: false` short-circuits to a no-op
- 🟢 `packages/buses/uarts/uart_hw_flow_ext.yaml` — declares the external component (mirrors `radio_probe_ext.yaml`)
- 🟢 `packages/buses/uarts/uart{1,2,3}_hwfc.yaml` — add `uart_hw_flow:` block wired to `${pin_uartN_cts}`/`${pin_uartN_rts}` + `enabled: ${uartN_hw_flow}`
- 🟢 `hw_defs/**/*.yaml` — add `uartN_hw_flow: false` next to each `uartN_baud` (MR4U, MRxU, 06xU, Ultima; slw09u has no radios)
- 🟢 `devices/{mr4u,mrxu,06xu,ultima}*.yaml` — include `uart_hw_flow_ext.yaml` before the `uartN_hw_flow.yaml` block

Success: `esphome compile mr4u-r1-73.yaml` links against `IDFUARTComponent::get_hw_serial_number()`; log line `[uart_hw_flow] Enabled: no` appears once per UART at boot with defaults. Flip a `uartN_hw_flow: true` locally and confirm `Enabled: yes` + `uart_set_hw_flow_ctrl` succeeds against a stock CTS-honoring radio image.

**Housekeeping deferred**: `packages/buses/uarts/uartN_no_hwfc.yaml` siblings are now truly dead (no device uses them). Collapse next time we're in this area (see [v1-design.md §9](v1-design.md) item 4).

---

## v1.x — stub replacement + quality of life

Additive to v1, no breaking changes. Each item is independent; order flexible.

### Replace Spinel stub with a real probe 🟢

Highest debugging cost of any protocol lives here (HDLC-lite framing + CCITT-16 CRC, unsolicited property notifications need discarding at boot). Landed as a post-v1 point release on `feature/spinel-probe`.

- 🟢 `components/radio_probe/protocol_helpers.h` — shared inline CCITT-16-FALSE CRC + HDLC escape/unescape, reusable by the pending EZSP probe
- 🟢 `components/radio_probe/spinel_probe.cpp` — HDLC-lite decoder, `PROP_NCP_VERSION` (prop 0x02), publishes the raw UTF-8 version string verbatim; catalog-normalisation is HA-template-side (see "rev is not uniformly YYYYMMDD" below)
- 🟢 `components/radio_probe/test/test_protocol_helpers.cpp` — host-compilable regression tests, ship as source
- ⚪ Verify against live MR4U EFR32 build once flashed (rebuild + observe `Radio 2 Installed Firmware` sensor)

### Replace EZSP stub with a real probe ⚪

- ⚪ `components/radio_probe/ezsp_probe.cpp` — EZSP over ASH framing (RFC 1662-style), send `version(0x00)` command, parse `stackVersion`
- ⚪ Wire test against MR4U Radio 2 flashed with EmberZNet coord (temporary reflash for validation, not shipped as default)

### Replace Z-Wave stub with a real probe ⚪

- ⚪ `components/radio_probe/zwave_probe.cpp` — Z-Wave Serial API `FUNC_ID_ZW_GET_VERSION` (0x15), parse `Library Version` string
- ⚪ Wire test against SLZB-Ultima ZW-800 (Radio 3)
- ⚪ **Regional-type enum in the SMLIGHT catalog.** The first snapshot ([../radio-firmware/](radio-firmware/)) revealed that Z-Wave `type` values are `5` (EU), `6` (US), `7` (ANZ) — encoding region, not role. This means Z-Wave needs an extra hw_defs axis (`radio*_zwave_region: eu | us | anz`) alongside the existing `radio*_role: primary_ctrl`. Update the compile-time validator and §6g permutation matrix accordingly. Currently only chip 12 (MRW10) carries these types; SLZB-Ultima's ZW-800 (chip ID TBD) is expected to follow the same pattern

### Nightly SMLIGHT catalog-diff CI 🔵

- 🔵 `.github/workflows/smlight-catalog-diff.yml` — cron fetch, diff against `docs/radio-firmware/catalog-snapshot.json`, open GitHub issue on new `(chip_id, type, prod)` tuples
- Purpose: early warning if SMLIGHT ships new firmware types (e.g. Matter, OTBR) we don't cover in §6g

### Auto-detect installed channel sensor ⚪

- ⚪ HA-side derived sensor per [v1-radio-firmware.md §7.5](v1-radio-firmware.md) — `prod` / `dev` / `custom`, derived from probed `rev` cross-referenced against catalog
- ⚪ Ship as an optional block in the HA snippet, not required

### Promote `firmware_channel` to HA `select` (conditional) 🔵

Only if v1 users prove they flip channels often enough that rebuild-per-change
is painful. `firmware_channel` is the one substitution that doesn't describe
flashed reality (see [v1-radio-firmware.md §10.6](v1-radio-firmware.md)), so
promoting it doesn't violate "selection == action" — protocol/role stay YAML.

- 🔵 Non-breaking: substitution stays as default, `select:` overrides at runtime

### Catalog-schema follow-ups (surfaced by the first snapshot, 2026-09-10) ⚪

Three real discrepancies the [committed snapshot](radio-firmware/) revealed
between the SMLIGHT ZB catalog and the assumptions baked into
[v1-radio-firmware.md](v1-radio-firmware.md) §§6g, 7. None block v1 shipping;
each turns an "always shows `custom`" or "silent filter drop" into a real
match.

#### `rev` is not uniformly YYYYMMDD ⚪

- ⚪ Chip 70 (CC2674P10) `rev` values *are* YYYYMMDD (e.g. `20260311`) — ZNP probe → catalog match works as designed
- ⚪ Chip 68 (EFR32MG26) `rev` values are SDK version strings — `"SDK v8.0.3"` (coord/router), `"2.7.2 sdk 2025.6.2"` (thread) — but the live Spinel probe returns the raw OpenThread `NCP_VERSION` build tag (e.g. `OPENTHREAD/thread-reference-20260416-…`). Equality match → never fires → every MG26 user would see `custom` on the auto-detect sensor and no upgrade offered on the update entity. Must be resolved before the SMLIGHT update-entity template lights up for Thread firmware.
- ⚪ Fix option (a): Jinja tolerant-match — regex-extract a comparable token from both sides, or substring match either direction
- ⚪ Fix option (b): accept that EFR32 auto-detect stays `custom` until v2 (when the flash chain owns the version write and we can cache what we flashed)
- ⚪ Cross-reference: same issue affects the "installed_firmware ↔ available_rev" comparison in the HA update template — the update entity will always show "up-to-date" for EFR32 because no candidate has an equal rev string

#### Missing `prod` field on many entries ⚪

- ⚪ Many older chip 70 entries lack the `prod` key entirely (out of 3 recent entries, all have it; out of 18 total, most older ones don't)
- ⚪ Current Jinja `selectattr('prod', 'eq', channel == 'prod')` drops any entry where `prod` is absent — silently
- ⚪ Decide desired semantics: (i) drop = "unknown provenance, don't offer" (current default), (ii) treat absent-as-false = "old entries are all dev", (iii) treat absent-as-true = "old public releases were all prod". Recommend (i) — least surprising, matches the "explicit is better than inferred" principle
- ⚪ Document the chosen semantics in [v1-radio-firmware.md](v1-radio-firmware.md) §7 next to the Jinja
- ⚪ Snapshot audit script that reports "N entries per chip lack `prod`" to inform the decision

#### Add chip 91 to the §6g permutation matrix ⚪

- ⚪ Snapshot surfaced chip **91** as a distinct EFR32MG24 codepoint separate from chip **67** — one entry only, ZREL Zigbee firmware (`type: 0`, no `hwFlow` field, `rev: 20260909`)
- ⚪ [v1-radio-firmware.md](v1-radio-firmware.md) §6g currently lists only 67 for MG24; add 91 with the observed (chip, protocol, role) tuple
- ⚪ Investigate: is chip 91 a hardware revision variant of MG24, a different SKU, or the SMLIGHT-branded "ZREL" packaging of the same chip? Determines whether the compile-time validator should treat 67/91 as aliases or distinct chips

#### Router-role entries have `baud: 0` ⚪

- ⚪ Every `type: "1"` (Zigbee router) entry in the catalog has `baud: 0` — routers participate in the mesh directly and don't speak a host UART protocol
- ⚪ Current Jinja `selectattr('baud', 'eq', uart_baud | int)` drops **every** router entry
- ⚪ Fix: filter conditional on `radio*_role` — if `router`, skip the baud match (or explicitly match `baud: 0`); if any other role, match `uart*_baud`
- ⚪ Update the pseudo-Jinja examples in [v1-radio-firmware.md](v1-radio-firmware.md) §7.3 and [v1-design.md](v1-design.md) §4 to show the conditional
- ⚪ Not urgent for MR4U (both radios are non-router) but blocks anyone selecting `radio*_role: router` before this is fixed

---

## v2 — flash chain + tier-3 UX

Goal: HA users can flash radio firmware without leaving HA. Only when this
lands does the "protocol/role as HA `select`" surface become honest (see
[v1-radio-firmware.md §10.5](v1-radio-firmware.md)).

**Design authority**: [design.md](design.md) §13 (v2 planning — pending write-up).

### Flash-chain core ⚪

- ⚪ HA add-on (separate repo) — queries SMLIGHT catalog, downloads image, verifies signature/checksum, drives our ESP32 to reflash the radio
- ⚪ Native API service on ESPHome side — enter/exit radio bootloader via `serial_proxy` DTR/RTS pins (already wired: `dtr_pin: ${pin_uartN_rst}`, `rts_pin: ${pin_uartN_flash}`)
- ⚪ Resume-safe write protocol (checksummed chunks, abort-and-rollback semantics)
- ⚪ Boot-time probe re-fires post-flash → HA sees updated `installed_firmware` immediately

### Tier-3 UX ⚪

- ⚪ HA `select` for `radioN_protocol`, `radioN_role`, `radioN_firmware_channel` — each pick triggers a real flash operation via the add-on
- ⚪ Options list computed from §6g permutation matrix per chip (dropdown never shows an invalid triple)
- ⚪ Explicit "confirm reflash" step before action; NVS-persisted post-flash
- ⚪ **`uartN_hw_flow` follows firmware channel automatically.** Today (v1) hwFlow is a static YAML substitution baked into the ESPHome build (see `hw_defs/*/*.yaml` → `radio1_uart_hw_flow`). That's correct for the shipped curated firmware set but breaks the moment the flash chain lets users switch to a channel with a different `hwFlow` value (e.g. a future ZNP build that enables CTS/RTS). v2 fix: derive the effective hwFlow from `(chip, firmware_channel)` via a compile-time lookup table mirroring the SMLIGHT catalog, reconfigure the ESP-IDF UART driver (`uart_set_hw_flow_ctrl()`) at the same NVS-persist point as channel selection, and expose a `Radio N UART flow control override` `select` (`default | force_off | force_on`) as an escape hatch for user-flashed exotic firmware. Component work already in place: `components/uart_hw_flow/` runs its own `setup()` and can be extended to reconfigure post-boot.

### Multi-PAN 🔵

EFR32 concurrent Zigbee EZSP + Thread Spinel on one chip.

- 🔵 Dual-protocol dispatch on the same UART (probe both, publish both sensors)
- 🔵 SMLIGHT catalog now lists multi-PAN builds under new `type` values — extend §6g matrix
- 🔵 Only worth building if user demand shows up

### Optional on-device dashboard 🔵

- 🔵 `web_server:` bolt-on package (opt-in) — for users who want a device-side UI for flash progress etc.
- 🔵 HTTPS story on ESP32 (self-signed vs ACME) — parked until v2 UX design lands
- 🔵 If HA add-on covers flash progress cleanly, this may never ship at all

### Security review — tier-3 config surface ⚪

Placeholder from [v1-radio-firmware.md §11](v1-radio-firmware.md):

- ⚪ If tier-3 selects go over Native API only, Noise + API key covers auth by construction — cheap win
- ⚪ If `web_server:` ships, do the HTTPS + CSRF review before merge
- ⚪ Flash-chain trust chain review: SMLIGHT signing (if present), image checksum verification, bootloader authentication path
- ⚪ Explicit non-goal: no on-device secret storage beyond what ESPHome already provides for the Native API key

---

## Ongoing / cross-cutting

### Keeping the §6g permutation matrix current ⚪

Silicon and protocol families are decade-stable; SMLIGHT could still add new
firmware types (e.g. Matter, OTBR) for existing chips. Two-layer defense:

1. **Compile-time validator** rejects unknown `(chip, protocol, role)` triples with a clear error — users can't silently misconfigure
2. **`docs/radio-firmware/catalog-snapshot.json`** — committed snapshot, refreshed manually via `docs/radio-firmware/refresh.py`. Review the git diff for new `(chip_id, type)` tuples on each refresh
3. **v1.x CI cron** (above) automates the "notice drift" step

Manual refresh cadence: bump when we prep a release, or when a user reports a "your matrix rejects my firmware" issue.

### Hardware map maintenance ⚪

- ⚪ Extend [hardware-map.md](hardware-map.md) as we validate additional devices
- ⚪ Add hw revision variants (`r1_74`, `r1_75`, …) as SMLIGHT ships them
- ⚪ Document the "add a new device" checklist once we've done it 2–3 times

### Vendor-security disclosure ⚪

A private security-findings review of SLZB-OS is maintained locally; disclosure is coordinated directly with SMLIGHT rather than published in this repo.

- ⚪ Draft private disclosure email to SMLIGHT
- ⚪ 90-day disclosure timer, offer to help test fixes

---

## Explicitly deferred / not doing

- ❌ **Sound-reactive WS2812 effects + I2S microphone (`music_leds` + `fastled_helper`, upstream from [`andrewjswan/esphome-components`](https://github.com/andrewjswan/esphome-components))** — deleted from the tree on 2026-09-10 along with the FastLED library dependency they pulled in. The pipeline (FFT every ~32 ms on a FreeRTOS task + FastLED rendering + WS2812 RMT output) fights the `serial_proxy` UART loops on the same ESP32-S3 that has to service 2–3 radio UARTs at 115200–460800 baud, causing byte drops on the Zigbee / Thread / Z-Wave streams. Core job wins. If sound-reactive effects ever come back, it will be gated behind an explicit runtime "pause radios while mic active" switch — not on-by-default. See [`design.md §18`](design.md) for the non-goal rationale and [`design.md §30.3`](design.md) for the removal register.
- ❌ **On-device SPA** — HA is the UX. If users want a device-side page for edge cases, `web_server:` covers it; we don't ship a custom UI.
- ❌ **On-device AI agent** — users can run whatever LLM they want in HA against our sensors.
- ❌ **Zigbee network-key management** — belongs in Z2M / ZHA, not in the transport.
- ❌ **Cloud firmware distribution of our own** — SMLIGHT's catalog is fine; we just fetch from it. No hosting our own.
- ❌ **Berry / user scripting on device** — automations live in HA. Keeps our attack surface minimal.
- ❌ **On-device file manager** — not shipping any HTTP file surface on the device.
- ❌ **Plaintext TCP fallback for `serial_proxy`** — Noise-encrypted or nothing. Users who want plaintext can run stock SLZB-OS.

---

## How to update this file

- Move items between sections in the same PR that ships (or defers) them.
- When something moves to 🟢 shipped, keep the bullet but note the release/tag it landed in.
- New ideas start as ⚪ planned or 🔵 stretch under the appropriate release.
- If an item gets rejected during design review, move it to *Explicitly deferred* with a one-line reason, not deletion — future contributors need to know we considered it.
