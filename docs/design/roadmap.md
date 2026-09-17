# Roadmap — slzb-esphome-ng

Living plan for what ships in each release, what's deferred, and what we've
explicitly decided not to do. Update this in the same PR as the code that
moves an item between sections.

Legend: 🟢 shipped · 🟡 in flight · ⚪ planned · 🔵 stretch · ❌ won't do

Cross-references:
- [design.md](design.md) — architecture and phase model
- [radio-probe-reference.md](radio-probe-reference.md) — radio version visibility (v1 authoritative design)
- [architecture.md](../architecture.md) — repo layout and package model

---

## v1 — probe + HA update notifications 🟡

Goal: users can see per-radio installed firmware in HA and get a native "update
available" card comparing against the SMLIGHT catalog. Zero flashing from us.
All radio-identity metadata is compile-time substitution — no runtime knobs.

Design in [radio-probe-reference.md](radio-probe-reference.md) §§3–7, §10. Status:
code + configuration merged; pending hardware validation on MR4U. Flip
🟡 → 🟢 once the log line + HA sensor value have been confirmed against
SLZB-OS's own reading.

### Step 1 — ZNP probe (MR4U Radio 1) 🟢

Prove the whole pattern on one radio, one protocol, one device.

- 🟢 `components/radio_probe/__init__.py` — codegen + cross-field validator enforcing §6g permutation matrix
- 🟢 `components/radio_probe/radio_probe.{h,cpp}` — component skeleton, `setup_priority = 250`, dispatcher on `protocol` enum
- 🟢 `components/radio_probe/znp_probe.cpp` — `SYS_VERSION` (0x2102), parse `TransportRev/Product/Major/Minor/Maint/Rev(LE u32)`, format `Rev` as YYYYMMDD
- 🟢 `packages/diagnostics/radio_probe_ext.yaml` + `radio{1,2,3}_probe.yaml` — template sensors from substitutions + `radio_probe:` instance publishing `installed_firmware` (split per-radio to match existing per-UART package pattern; UART id parametrized via `radioN_uart_id` substitution so single-radio boards like 06xU on UART2 still surface as "Radio 1")
- 🟢 `hw_defs/mrxu/mr4u_r1_73.yaml` — add `radio1_*` substitutions (chip, protocol=`znp`, role=`coord`, firmware_channel=`dev`)
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
- 🟢 Top-level [README.md](../../README.md) "Radio firmware version visibility in HA" row now points at `docs/ha-integrations/` instead of the design draft; radio-firmware feature bullet and comparison table both scoped to "ZNP live in v1, other protocols stubbed" so nothing overpromises

### Step 5 — UART hardware flow control component 🟢

Upstream ESPHome's `uart:` platform has no YAML surface for classic RTS/CTS handshake (`flow_control_pin` is for RS485 driver-enable only). Without this component the `hwFlow` filter axis in the HA update catalog Jinja is a lie — we'd offer firmware images the ESP32 side can't actually honor. Ships as v1 for correctness-by-construction, even though every stock SMLIGHT catalog entry currently visible to MR4U has `hwFlow` absent/false.

- 🟢 `components/uart_hw_flow/{__init__.py, uart_hw_flow.h, uart_hw_flow.cpp}` — `Component` at `setup_priority 250`; static_casts parent to `uart::IDFUARTComponent`, calls `uart_set_pin()` + `uart_set_hw_flow_ctrl(port, UART_HW_FLOWCTRL_CTS_RTS, 122)` on `setup()`; `enabled: false` short-circuits to a no-op
- 🟢 `packages/buses/uarts/uart_hw_flow_ext.yaml` — declares the external component (mirrors `radio_probe_ext.yaml`)
- 🟢 `packages/buses/uarts/uart{1,2,3}_hw_flow.yaml` — add `uart_hw_flow:` block wired to `${pin_uartN_cts}`/`${pin_uartN_rts}` + `enabled: ${uartN_hw_flow}`
- 🟢 `hw_defs/**/*.yaml` — add `uartN_hw_flow: false` next to each `uartN_baud` (MR4U, MRxU, 06xU, Ultima; slw09u has no radios)
- 🟢 `devices/{mr4u,mrxu,06xu,ultima}*.yaml` — include `uart_hw_flow_ext.yaml` before the `uartN_hw_flow.yaml` block

Success: `esphome compile mr4u-r1-73.yaml` links against `IDFUARTComponent::get_hw_serial_number()`; log line `[uart_hw_flow] Enabled: no` appears once per UART at boot with defaults. Flip a `uartN_hw_flow: true` locally and confirm `Enabled: yes` + `uart_set_hw_flow_ctrl` succeeds against a stock CTS-honoring radio image.

**Housekeeping done**: dead `packages/buses/uarts/uartN_no_hwfc.yaml` siblings deleted (no device included them).

Design in [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §5.

---

## v1.x — stub replacement + quality of life

Additive to v1, no breaking changes. Each item is independent; order flexible.

### Replace Spinel stub with a real probe 🟢

Highest debugging cost of any protocol lives here (HDLC-lite framing + CCITT-16 CRC, unsolicited property notifications need discarding at boot). Landed as a post-v1 point release on `feature/spinel-probe`.

- 🟢 `components/radio_probe/protocol_helpers.h` — shared inline CCITT-16-FALSE CRC + HDLC escape/unescape, reusable by the pending EZSP probe
- 🟢 `components/radio_probe/spinel_probe.cpp` — HDLC-lite decoder, `PROP_NCP_VERSION` (prop 0x02), publishes the raw UTF-8 version string verbatim; catalog-normalisation is HA-template-side (see "rev is not uniformly YYYYMMDD" below)
- 🟢 `components/radio_probe/test/test_protocol_helpers.cpp` — host-compilable regression tests, ship as source
- ⚪ Verify against live MR4U EFR32 build once flashed (rebuild + observe `Radio 2 Installed Firmware` sensor)

### Replace EZSP stub with a real probe — deferred ⚪

- ⚪ `components/radio_probe/ezsp_probe.cpp` — EZSP over ASH framing (RFC 1662-style), send `version(0x00)` command, parse `stackVersion`
- ⚪ Wire test against MR4U Radio 2 flashed with EmberZNet coord (temporary reflash for validation, not shipped as default)
- **Deferred beyond v1.x point releases** until an EZSP-based target device is in maintainer hands. None of the currently-supported devices ship an EFR32 coord by default (MR4U/Ultima ship MG26 dev-Thread on Radio 2, id 13). Building the probe now would ship untested code. Track as "unblocked when we add a stock SLZB-06 hub target that runs EZSP by default", or when a maintainer flashes MR4U Radio 2 to id-21/68 EZSP for a validation cycle.

### Replace Z-Wave stub with a real probe ⚪

- ⚪ `components/radio_probe/zwave_probe.cpp` — Z-Wave Serial API `FUNC_ID_ZW_GET_VERSION` (0x15), parse `Library Version` string
- ⚪ Wire test against SLZB-Ultima ZW-800 (Radio 3)
- ⚪ **Regional-type enum in the SMLIGHT catalog.** The first snapshot ([../radio-firmware/](../radio-firmware/)) revealed that Z-Wave `type` values are `5` (EU), `6` (US), `7` (ANZ) — encoding region, not role. This means Z-Wave needs an extra hw_defs axis (`radio*_zwave_region: eu | us | anz`) alongside the existing `radio*_role: primary_ctrl`. Update the compile-time validator and §6g permutation matrix accordingly. Currently only chip 12 (MRW10) carries these types; SLZB-Ultima's ZW-800 (chip ID TBD) is expected to follow the same pattern

### Nightly SMLIGHT catalog-diff CI 🔵

- 🔵 `.github/workflows/smlight-catalog-diff.yml` — cron fetch, diff against `docs/radio-firmware/catalog-snapshot.json`, open GitHub issue on new `(chip_id, type, prod)` tuples
- Purpose: early warning if SMLIGHT ships new firmware types (e.g. Matter, OTBR) we don't cover in §6g

### Auto-detect installed channel sensor ⚪

- ⚪ HA-side derived sensor per [radio-probe-reference.md §7.5](radio-probe-reference.md) — `prod` / `dev` / `custom`, derived from probed `rev` cross-referenced against catalog
- ⚪ Ship as an optional block in the HA snippet, not required

### Promote `firmware_channel` to HA `select` (conditional) 🔵

Only if v1 users prove they flip channels often enough that rebuild-per-change
is painful. `firmware_channel` is the one substitution that doesn't describe
flashed reality (see [radio-probe-reference.md §10.6](radio-probe-reference.md)), so
promoting it doesn't violate "selection == action" — protocol/role stay YAML.

- 🔵 Non-breaking: substitution stays as default, `select:` overrides at runtime

### Catalog-schema follow-ups ⚪

Three real discrepancies the [committed snapshot](../radio-firmware/) revealed
between the SMLIGHT ZB catalog and the assumptions baked into
[radio-probe-reference.md](radio-probe-reference.md) §§6g, 7. None block v1 shipping;
each turns an "always shows `custom`" or "silent filter drop" into a real
match. Full audit inventory (per-id chip families, duplicate SHA groups,
broken links, baud/hwFlow matrix): see
[docs/radio-firmware/catalog-audit.md](../radio-firmware/catalog-audit.md).

#### `rev` is not uniformly YYYYMMDD ⚪

- 🟢 **Resolved for MR4U by dropping the `smlight_id` axis entirely.** The initial mapping (chip 68, SMHUB signed EFR32MG26 track with SDK-string `rev`) was incorrect; MR4U's flashed image comes from chip 13 (`slzb06Mg26/*/slzb06Mg26_openthread_rcp_*.gbl`) whose `rev` field IS YYYYMMDD (e.g. `20260416`) and matches the live Spinel probe by direct string equality. Chip 4 (CC2674P10 dev track, `slzb06p10/*/znp-*.bin`) similarly aligns for Radio 1. See radio-probe-reference.md §6h for the empirical audit. The HA template now maps `chip` → the chip family's duplicate-SHA equivalence group (see [radio-firmware/catalog-audit.md](../radio-firmware/catalog-audit.md)). The signed-track SDK-string concern still exists for any future device pinned to id 68 (or the signed CC2674P10 id 70) — but nothing this fork ships today is on those tracks.
- ⚪ (Residual, non-blocking) Chip 91 (EFR32MG24 ZREL Zigbee) `rev` values may be SDK-style; verify next time we add MG24 hardware
- ⚪ Fix option (a): Jinja tolerant-match — regex-extract a comparable token from both sides, or substring match either direction
- ⚪ Fix option (b): accept that any signed-track auto-detect stays `custom` until v2 (when the flash chain owns the version write and we can cache what we flashed)
- ⚪ Cross-reference: same issue would affect "installed_firmware ↔ available_rev" comparison in the HA update template for devices explicitly pinned to signed tracks

#### Missing `prod` field on many entries 🟢

- 🟢 **Resolved: shipped semantics documented in [radio-probe-reference.md](radio-probe-reference.md) §7.4.** 33/169 entries (20%) omit `prod` — primarily older CC26xx ids (0, 3, 4, 5, 16, 17, 18) and SLZB-06m id 1. Current Jinja `selectattr('prod', 'eq', channel == 'prod')` silently drops absent-`prod` entries; rationale is "explicit is better than inferred" — the alternatives (absent = false, absent = true) each silently reclassify ~20% of the catalog. Catalog-diff CI cron (below) will surface if SMLIGHT starts pruning `prod` from newer entries.

#### Add chip 91 to the §6g permutation matrix 🟢

- 🟢 **Done** — [radio-probe-reference.md](radio-probe-reference.md) §6g now lists 91 alongside 23/65/67 for EFR32MG24 (line 480). Snapshot surfaced chip 91 as a single-entry ZREL Zigbee firmware (`type: 0`, no `hwFlow`, `rev: 20260909`); treated as an alias of MG24 rather than a distinct chip family (bytes differ from id 23 but silicon is the same per SLZB device docs). Compile-time validator accepts the tuple `(efr32mg24, ezsp, coord)` uniformly for both ids.
- ⚪ (Residual) Byte-content vs id 23 not yet SHA-verified for the 91 entry; check next MG24 hardware validation cycle

#### Router-role entries — mixed `baud: 0` and `baud: 115200` 🟢

- 🟢 **Shipped in [ha-integrations/smlight-firmware-update.yaml](../ha-integrations/smlight-firmware-update.yaml).** The three catalog filter chains (`latest_version`, `release_summary`, `release_url`) now compute `allowed_bauds = [0, baud] if role == 'router' else [baud]` and use `selectattr('baud', 'in', allowed_bauds)`.
- Empirical basis: 17/30 `type: "1"` entries carry `baud: 0`; 13 (older CC26xx ids) carry `baud: 115200`. The Jinja now accepts either variant for router role.
- ⚪ Pseudo-Jinja examples in [radio-probe-reference.md](radio-probe-reference.md) §7.3 and [radio-firmware-mgmt.md](radio-firmware-mgmt.md) §4 still show the single-baud form; update next time those sections are touched (design record, not shipped code).
- ⚪ MR4U doesn't exercise the fix (both radios non-router); first real test lands when a user selects `radio*_role: router`.

---

## v2 — flash chain + tier-3 UX

Goal: HA users can flash radio firmware without leaving HA. Only when this
lands does the "protocol/role as HA `select`" surface become honest (see
[radio-probe-reference.md §10.5](radio-probe-reference.md)).

Design in [design.md](design.md) §26.6 (implementation shape: Python vs YAML vs HA add-on).

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

### Radio maintenance actions (Phase 7) ⚪

The v2 add-on becomes the UX host for the authenticated ESPHome-side actions
that replace SLZB-OS's plaintext HTTP admin surface for per-radio operations
([design.md §15](design.md)). Every action requires the ownership guard
([design.md §14](design.md)): ZHA/OTBR must be stopped or force-released
before invocation. Phasing mirrors [design.md §26.6.4](design.md).

- ⚪ **v2.1 — Zigbee network backup export.** nwk key + PAN + device table dump. Same ZNP session as the flash chain uses. Written to `/config/backups/` next to HA snapshots.
- ⚪ **v2.2 — IEEE migration (the coord-swap star).** `zigbee_ieee_read` on the outgoing coord → `zigbee_ieee_write` on the incoming coord. ZNP MT_SYS CMD 12/14 read, CMD 11 write. Preserves every Zigbee pairing across a coordinator replacement — without this, users re-pair every device. Full migration flow lives in [design.md §29.2](design.md). Wrong value bricks discovery until reflash; add-on UX must guard with an explicit "you are about to overwrite the coord IEEE" confirmation.
- ⚪ **v2.3 — Radio SoC die temperature diagnostic.** ZNP `SYS_GET_MFG_INFO` (or Silabs equivalent) polled on the add-on's schedule; publishes as an HA sensor.
- ⚪ **v2.4 — Zigbee energy scan + link diagnostics.** ZNP `ZDO_MGMT_NWK_UPDATE_REQ`. Channel-picker helper, RSSI/link snapshot, radio TX-power read (setter stays with ZHA/Z2M — see [design.md §15](design.md)).
- ⚪ **Radio reset (`zigbee_radio_reset` / `thread_radio_reset`).** Not tied to a specific v2.x — reuses the same GPIO the flash chain drives (`serial_proxy` `dtr_pin`). Ships whenever it's needed as a manual "kick the radio" escape hatch.
- ⚪ **Docs**: add a "Migrating a coordinator (preserve pairings)" recipe under [`docs/ha-integrations/`](../ha-integrations/) that walks users through the stop-ZHA / read-IEEE / swap / write-IEEE / restart-ZHA sequence.

### Optional on-device dashboard 🔵

- 🔵 `web_server:` bolt-on package (opt-in) — for users who want a device-side UI for flash progress etc.
- 🔵 HTTPS story on ESP32 (self-signed vs ACME) — parked until v2 UX design lands
- 🔵 If HA add-on covers flash progress cleanly, this may never ship at all

### Security review — tier-3 config surface ⚪

Placeholder from [radio-probe-reference.md §11](radio-probe-reference.md):

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

- ⚪ Extend [hardware-map.md](../hardware-map.md) as we validate additional devices
- ⚪ Add hw revision variants (`r1_74`, `r1_75`, …) as SMLIGHT ships them
- ⚪ Document the "add a new device" checklist once we've done it 2–3 times

### Vendor-security disclosure ⚪

A private security-findings review of SLZB-OS is maintained locally; disclosure is coordinated directly with SMLIGHT rather than published in this repo.

- ⚪ Draft private disclosure email to SMLIGHT
- ⚪ 90-day disclosure timer, offer to help test fixes

---

## Explicitly deferred / not doing

- ❌ **Sound-reactive WS2812 effects + I2S microphone (`music_leds` + `fastled_helper`, upstream from [`andrewjswan/esphome-components`](https://github.com/andrewjswan/esphome-components))** — deleted from the tree along with the FastLED library dependency they pulled in. The pipeline (FFT every ~32 ms on a FreeRTOS task + FastLED rendering + WS2812 RMT output) fights the `serial_proxy` UART loops on the same ESP32-S3 that has to service 2–3 radio UARTs at 115200–460800 baud, causing byte drops on the Zigbee / Thread / Z-Wave streams. Core job wins. If sound-reactive effects ever come back, it will be gated behind an explicit runtime "pause radios while mic active" switch — not on-by-default. See [`design.md §18`](design.md) for the non-goal rationale and [`design.md §30.3`](design.md) for the removal register.
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
