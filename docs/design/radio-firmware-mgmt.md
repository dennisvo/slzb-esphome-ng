# Radio firmware management — design considerations

Status: **implemented** — v1 code + configuration are shipped; hardware
validation on MR4U is the only remaining gate. Captures decisions and
rationale from the firmware-update investigation. Serves as the paper trail
for future implementation work.

Scope of this doc:

- Radio-firmware management (§1–§10): what update UX we ship, how role
  switching is handled, how firmware flashing might work later.

Feature-parity and HA-integration-shape content that used to live here
(old §11–§13) has moved into [design.md](design.md) §27, §28, and §26.6 —
see the pointer at the bottom of this file.

Implementation-level details (snippets, exact filenames) live in
[radio-probe-reference.md](radio-probe-reference.md).

---

## 1. Scope decisions

### In scope for v1

- **Radio firmware update check** — HA `update` entity per radio, comparing installed version to SMLIGHT's public JSON catalog.
- **Radio metadata as ESPHome diagnostics** — chip, firmware type, baud, HW flow, firmware channel. Static per hardware revision, encoded in `hw_defs/**.yaml`.
- **Live installed-version probe** — ESPHome speaks ZNP `SYS_VERSION` to the radio at boot, publishes the real version as a text_sensor. In-memory only, no cached fallback. See radio-probe-reference.md §6 for the frame layout.
- **HW flow control custom component** — small custom component that calls `uart_set_hw_flow_ctrl()` on the ESP-IDF UART HAL. Enables HW flow when the radio firmware requires it. See §5.
- **Feature-parity entities** from SMLIGHT's HA integration where they're cheap to add and useful — see [design.md §27.1](design.md#271-smlight-ha-integration--entity-portskip-walkthrough) for the full port/skip table.

### Beyond v1

Future-scope items (one-click flash, Thread live probe, radio-maintenance actions, IEEE migration, network backup, energy scan, die temperature) are tracked in [roadmap.md](roadmap.md) v1.x/v2/v3.

### Non-goals

Architectural non-goals for the entire fork — including runtime role-switching, on-device OTBR / Matter / Zigbee Hub, cloud firmware pull, and radio-side sensors that would require a persistent second UART channel — are collected in [design.md §18](design.md#18-non-goals-for-v1). SLZB-OS-specific product features surveyed and their disposition (kept / retained-off / removed / out-of-scope) live in [design.md §28](design.md#28-slzb-os-features-surveyed-disposition).

---

## 2. Architectural principle: static device configuration

**Every property of the device that affects wire-level behaviour lives in the
YAML build config and is set at compile time.** No runtime mutation of pins,
baud, flow control, or radio role.

Consequences:

- One (chip, firmware-role, baud, hwFlow) combination per build.
- Role change or baud change ⇒ edit YAML ⇒ recompile ⇒ OTA. Not a one-click operation.
- ESPHome, `serial_proxy`, ZHA, and Z2M all see a stable UART configuration for the life of a firmware image.
- The device becomes a hardware appliance with an explicit config, not a general-purpose radio management platform.

This mirrors how ESPHome treats every other device family — the YAML *is* the configuration. It also mirrors the practical reality: role changes and baud changes happen once every few years for most users, not every week.

---

## 3. What lives where

| Property | Home | Mutable at runtime? | Notes |
|---|---|---|---|
| Chip on each UART | `hw_defs/**.yaml` | ❌ | Physical fact |
| Radio protocol (znp / spinel / ezsp / zwave / none) | `hw_defs/**.yaml` | ❌ | Compile-time. Determines which wire language the boot probe speaks |
| Radio role (coord / router / rcp / ncp / primary_ctrl) | `hw_defs/**.yaml` | ❌ | Compile-time. Maps 1:1 to SMLIGHT catalog `type` field |
| UART baud | `hw_defs/**.yaml` (already there) | ❌ | Must match installed firmware's baud |
| UART hardware flow | `uart*_hw_flow` (hw_defs substitution) + custom `uart_hw_flow` component | ❌ | Must equal firmware's `hwFlow`. See §5 |
| Currently-installed radio FW version | ESPHome live probe at boot, in-memory `text_sensor` | ❌ | Truthful, no cache. Publishes `unknown` on probe failure |
| Latest available radio FW version | HA `rest:` sensor against SMLIGHT catalog | ✅ polled | 24h refresh |
| Update-check channel (Prod / Dev) | `hw_defs/**.yaml` substitution `firmware_channel` | ❌ | Compile-time; edit YAML + rebuild to change. Values: `prod` \| `dev` (matches the SMLIGHT catalog's `prod: bool` field verbatim) |

### Sidebar: why no cached "installed firmware" file

SLZB-OS caches the installed radio firmware version to `/zbVer.txt` and `/zbVer2.txt`
(space-separated `<version> <type> <channel>`), written by the `WRITE_RADIO_INFO`
action right after every successful flash. That's fine when SLZB-OS owns the flashing
chain end-to-end and no other actor writes to the radio. It becomes actively misleading
the moment a user flashes out-of-band — with cc2538-bsl.py, silabs-firmware-flasher,
or any other host tool — because the cache is now stale but the device confidently
reports the wrong version to HA.

Our v1 rejects the cache and lives with a 1–2 s boot-time ZNP probe instead. The
tradeoff flips once we ship v2 one-click flash: if the fork becomes the only sanctioned
flashing path, a `zbVer.txt`-style cache is safe *provided* we also expose a
"re-probe now" button as an escape hatch for users who did flash externally.
Concrete rule for whenever we revisit: **cache only if the same code path that writes
the cache is the only supported way to flash the radio.**

### Sidebar: radio TX power lives with the Zigbee client, not us

TX power is a Zigbee/Thread *stack* setting, not a UART / host-firmware setting.
It gets applied by whoever is speaking ZNP or Spinel to the radio at coordinator
startup — not by anything the ESP32 does or could do without breaking the
`serial_proxy` dumb pipe. So it never appears in our YAML, in `hw_defs/**.yaml`,
or in the fork's HA entity surface. Users configure it in the client:

| Client | Where TX power is configured |
|---|---|
| Zigbee2MQTT | `advanced.transmit_power: <dBm>` in `configuration.yaml` |
| ZHA | `zigpy_config.tx_power` in HA `configuration.yaml` |
| OpenThread Border Router | `ot-ctl txpower <dBm>` or the Thread commissioning API |

Reference points: SLZB-OS itself makes the same choice for LAN mode — the main
UI has no TX power control, and the value only surfaces on the *internal* Zigbee
Hub config page (which we're not porting; see [design.md §28](design.md#28-slzb-os-features-surveyed-disposition)). If we ever want to expose
"currently applied TX power" as a **diagnostic** we can add a v2-add-on-side ZNP
`SYS_GET_TX_POWER` query and publish it as a read-only text sensor. Never the
setter — that stays with the client.

---

## 4. Filter chain — matching a catalog entry to a device

An entry in SMLIGHT's `type=ZB` catalog is uniquely identified by six properties we care about. All six must match for us to consider it a valid upgrade candidate:

| Catalog field | Source of truth on our side | Notes |
|---|---|---|
| Top-level key (chip ID) | `radio*_chip` (hw_defs) → HA-side chip_to_id map | e.g. `cc2674p10` → `"4"` for CC2674P10 dev-Zigbee |
| `type` | `radio*_role` (hw_defs) | `coord`→`"0"`, `router`→`"1"`, `rcp`/`ncp`→`"2"`. See [radio-probe-reference.md](radio-probe-reference.md) §6g permutation matrix |
| — | `radio*_protocol` (hw_defs) | Not a catalog filter axis — selects which wire-protocol probe runs. Cross-validated at compile time against `role` per §6g |
| `baud` | `uart*_baud` (hw_defs, already present) | e.g. `115200`, `460800` |
| `hwFlow` (default false when absent) | `uart*_hw_flow` (hw_defs substitution) | Must equal the firmware's `hwFlow`. See §5 |
| `prod` (bool) | `firmware_channel` (hw_defs substitution) | Substitution value matches the catalog field name verbatim: `prod` ⇒ filter for `prod: true`, `dev` ⇒ filter for `prod: false`. The ZB catalog only carries `prod`; some feeds also emit `dev: true` but we don't rely on it. Note: some ZB entries omit `prod` entirely — see [roadmap.md](roadmap.md) v1.x for the "missing `prod` treatment" follow-up |

**HW flow must be an exact match on both ends** — firmware says no, we set no; firmware says yes, we set yes. Mismatch either hangs the ESP32 (waiting on CTS the radio never asserts) or overruns the radio (radio asserts CTS but ESP32 ignores). See §5 for the ESP32 side of enabling it.

**Multiple entries with different bauds do exist for the same (chip, type)
combination.** Verified from the catalog: CC2674P10 coord ships as both
`20260310` @ 460800 baud and `20260311` @ 115200 baud, described as literally
the same build at different serial rates. Without the baud filter we'd offer
users incompatible firmware.

Pseudo-Jinja for the filter (real template lives in the HA snippet):

```jinja
{% set entries = state_attr('sensor.smlight_firmware_catalog_raw', chip_id) or [] %}
{% set match = entries
    | selectattr('type',    'eq', role_to_type)
    | selectattr('baud',    'eq', uart_baud | int)
    | selectattr('prod',    'eq', firmware_channel == 'prod')
    | selectattr('hwFlow',  'eq', uart_hw_flow | default(false))
    | list %}
{{ (match | sort(attribute='rev', reverse=true) | first).rev if match else installed_version }}
```

`role_to_type` is derived on the HA side by a small mapping (`coord`→`"0"`, `router`→`"1"`, `rcp`/`ncp`→`"2"`).

Note: the catalog's `hwFlow` field is *optional* — when absent, treat as `false`. The Jinja above uses `selectattr('hwFlow', 'eq', ...)` which requires the attribute present; the real HA template needs to normalise `hwFlow` to `false` when absent before comparing.

When the filter returns no matches, the template falls back to `installed_version` — the update entity goes silent. See §5.

---

## 5. The baud wrinkle

**The problem in one sentence**: a firmware update might require a UART re-config (baud), and that re-config is a compile-time property of our ESPHome build.

Consequences and decisions:

1. **Filter catches most cases.** If the user has hw_defs pinned to `uart1_baud: 115200`, we only surface 115200-baud firmware. If SMLIGHT ships a 460800-only future release for the selected channel, our filter returns nothing.

2. **When no matching entry exists, stay silent.** The update entity shows "up-to-date" because there is no compatible upgrade candidate. This is the simplest and least confusing behaviour — users who suspect they're missing an update can check the catalog manually, but we don't inject "update-available-but-blocked" states into their HA UI.

3. **Config-change updates are a documented manual workflow**:
   1. Bump `uart1_baud` in your local YAML
   2. `esphome run mr4u-local.yaml` — OTA the new ESPHome image
   3. THEN flash the new radio firmware (manual or v2's one-click flow)
   
   Order matters — reverse it and the running image can't talk to the radio between steps 2 and 1.

4. **Don't try to auto-detect and adapt.** ESPHome could in principle re-init the UART at a different baud based on a runtime value. Please don't. That's the "clever" path that breaks in ways no one can diagnose from logs. Static config wins.

### Recommended baud per radio

**Radio 1 — CC2674P10 (chip 70) as Coordinator: pin `uart1_baud: 115200`.**

- Matches every published SLZB-06P10 coord firmware except the two March 2026 460800 test builds.
- 460800 for coord is described in the catalog as "first ever test build" — bleeding edge.
- 115200 is the industry default for TI Zigbee dongles (Sonoff ZBDongle-E, ConBee-equivalents, all stock TI adapters).
- Coord traffic doesn't need the throughput.

**Radio 2 — EFR32MG26 (chip 68) as Thread/OT-RCP: pin `uart2_baud: 460800`.**

- 100% of Thread firmware in SMLIGHT's catalog is 460800. There is no 115200 Thread build for any chip.
- Chip 13 Thread entries declare `hwFlow: true` (MG26 dev-Thread track — MR4U Radio 2). Chips 65/67 (EFR32MG24) do too — that's a future-device concern. See radio-probe-reference.md §6h.
- Headroom at 460800 without HW flow: ESP32-S3 128B FIFO + ESPHome 256B buffer = ~8.3 ms of buffering. Fine for our minimal MR4U build; would tighten if we later added BLE-proxy or other heavy components on the same core.

Revisit both if SMLIGHT deprecates the current defaults, or when adding a chip that requires HW flow at 460800 (e.g. SLZB-06M's MG24).

### Note on HW flow control

**Hardware supports it.** Every SLZB device in this repo (06xu, mrxu, mr4u, ultima) has CTS/RTS wired to dedicated GPIOs — see any `hw_defs/**.yaml`. That's why SLZB-OS can expose a toggle in its web UI: same silicon, different firmware. SLZB-OS is a custom ESP-IDF firmware and calls `uart_set_hw_flow_ctrl()` directly on the ESP32 UART peripheral.

**ESPHome YAML doesn't expose it.** The `flow_control_pin` option in ESPHome's `uart:` component is for RS485 driver-enable, not for classic RTS/CTS handshake.

**We ship a custom component.** Shape:

```yaml
external_components:
  - source: components  # this repo
    components: [uart_hw_flow]

uart_hw_flow:
  - uart_id: hw_uart1
    cts_pin: ${pin_uart1_cts}
    rts_pin: ${pin_uart1_rts}
    enabled: ${uart1_hw_flow}   # "true" or "false", pulled from hw_defs
```

Internal implementation (`components/uart_hw_flow/`): a plain `Component` at `setup_priority 250` — after `esphome::uart` initialises the ESP-IDF UART driver at ~1000, before serial_proxy attaches at AFTER_CONNECTION (~-30). On `setup()` it looks up the port number via `UARTComponent::get_hw_serial_number()`, calls `uart_set_pin()` to route CTS/RTS through the GPIO matrix, then `uart_set_hw_flow_ctrl(port, UART_HW_FLOWCTRL_CTS_RTS, 122)`. `enabled: false` short-circuits to a no-op so the YAML shape stays stable regardless of the substitution value. About 60 lines of C++ with guard-rails.

**Matching rule (critical)**: `uart*_hw_flow` in hw_defs *must* equal the installed radio firmware's `hwFlow`. Enforced by the update filter chain (§4). Mismatch modes:

- Firmware `hwFlow: false`, ESP32 HW flow ON ⇒ ESP32 waits on CTS the radio never asserts. Symptom: TX from ESP32 hangs.
- Firmware `hwFlow: true`, ESP32 HW flow OFF ⇒ Radio asserts CTS on buffer pressure; ESP32 ignores. Symptom: dropped bytes on radio RX under bursts. Hard to diagnose.

**Decision matrix** (from catalog inspection):

| Scenario | Baud | Firmware hwFlow | `uart*_hw_flow` |
|---|---|---|---|
| CC2674P10 coord (MR4U Radio 1, current) | 115200 | false | `false` |
| EFR32MG26 Thread (MR4U Radio 2) | 460800 | false | `false` |
| EFR32MG24 Thread (future SLZB-06M) | 460800 | **true** | **`true`** |
| CC2674P10 Thread (chip 70 OT-RCP) | 460800 | false | `false` |

**Bottom line for MR4U today: value in `uart*_hw_flow` is `false` on both radios.** The custom component still ships in v1 so the substitution exists and future MG24 support drops in cleanly.

**Safety belt against future misconfigs.** If `uart*_hw_flow: true` is ever set while the peer firmware has `hwFlow: false`, the ESP32 UART peripheral stalls waiting on a CTS the peer never drives. The `radio_probe` boot-time probe defends against this — it uses a bounded `uart_wait_tx_done` and delayed dispatch so the probe reports `unknown (…)` with an error-level log naming this exact mismatch, rather than hanging the main task and triggering `safe_mode` OTA rollback. See [radio-probe-reference.md §6d "Boot-time hazard"](radio-probe-reference.md#boot-time-hazard--bounded-flush--delayed-dispatch).

Shipped in `packages/buses/uarts/uart{1,2,3}_hw_flow.yaml` — UART block + `uart_hw_flow:` invocation. The `_no_hwfc.yaml` siblings are dead files pending deletion (housekeeping in §9).

---

## 6. Role-switching — why we're not building it

Considered and rejected for v1:

### The naive design

- HA `input_select`: "Radio 1 desired role" → coord / router / thread
- User picks a new role → HA offers the corresponding catalog entry as "update available"
- Install button flashes the new firmware
- ESPHome refreshes and now runs the new role

### Why it's harder than it looks

1. **UART config is compile-time.** Coord and Thread firmwares for MR4U run at different bauds (115200 vs 460800). Changing role therefore requires an ESPHome rebuild AND a radio flash. Sequenced right, this works. Sequenced wrong, comms die. That's a foot-gun to expose as a one-click action.
2. **ZHA/Z2M integration state depends on role.** ZHA is configured with `radio_type: znp` (coord). Switching Radio 1 to a router means ZHA needs re-pointing to Radio 2 instead, or removed entirely. That's not a firmware operation, it's a HA reconfiguration operation, and it's out of our control.
3. **The "why" isn't compelling for most users.** People who bought a coordinator want a coordinator. The one-in-a-hundred user who wants to switch to router mode can absolutely do so — by editing `mr4u-local.yaml`, recompiling, and flashing manually. That's a 15-minute exercise, once, ever.

### The right shape if we ever do build it

Not a runtime feature. A documented workflow with helper packages:

- `packages/roles/mr4u_radio1_coord.yaml` — sets up substitutions + serial_proxy for coord on Radio 1
- `packages/roles/mr4u_radio1_router.yaml` — same but router (may not need serial_proxy if router doesn't have a HA-side stack)
- `packages/roles/mr4u_radio1_thread.yaml` — thread + OTBR guidance
- Users pick which to `!include` — done at YAML compose time, no runtime state

This is trivial to add later if demand appears. It doesn't need to be in v1.

### User responsibility (documentation contract)

When a user changes `radio*_protocol`, `radio*_role`, `uart*_baud`, or `uart*_hw_flow` (or the corresponding `packages/roles/*.yaml` include) in their local YAML:

1. **They are responsible for flashing matching radio firmware before the next boot.** All four axes must line up: firmware protocol ↔ `radio*_protocol`, firmware role ↔ `radio*_role`, firmware baud ↔ `uart*_baud`, firmware `hwFlow` ↔ `uart*_hw_flow`. The wire-protocol-vs-role split is documented in [radio-probe-reference.md §6i](radio-probe-reference.md#6i-wire-derived-chip--role--considered-and-rejected) — role is a stack-side property inside the flashed image, not a wire-level negotiation; the same wire protocol supports multiple roles.
2. If they build+OTA our fork without also reflashing the radio, the running image will attempt to speak (say) ZNP to a chip running Thread — no HA-side error, just silence. Same class of failure for baud or HW-flow mismatch.
3. The live-boot probe helps catch this: if the probe times out because the frame protocol or line settings don't match, the diagnostic `radio*_installed_firmware` sensor publishes `unknown` — a visible signal that ESPHome's declared configuration doesn't match the chip's real firmware.
4. **Chip and role verification is on the operator.** v1 does not publish wire-derived `chip_probed` / `role_probed` sensors — an earlier draft did, but flashed testing showed the wire evidence we could extract wasn't accurate enough to earn the complexity (see [radio-probe-reference.md §6i](radio-probe-reference.md#6i-wire-derived-chip--role--considered-and-rejected)). The declared `radio*_chip` / `radio*_protocol` / `radio*_role` sensors publish the values baked into `hw_defs/**` as the authoritative identity; the practical misconfig detector is the wire-derived `installed_firmware` rev landing (or failing to land) inside the SMLIGHT catalog track HA iterates for the update entity.
5. **Catalog-fit disagreements** (installed rev not present in the catalog, baud / hw_flow filter miss, chip family in a byte-identical duplicate SHA group) are computed HA-side against the fetched SMLIGHT catalog and surfaced only through the update entity — not as separate device-side sensors. The catalog is HA-fetched daily and can change without a device reboot, so keeping catalog-fit judgement HA-side avoids overclaiming device knowledge. See [radio-firmware/catalog-audit.md](../radio-firmware/catalog-audit.md) for the duplicate-SHA groups the HA Jinja iterates over.

**README warning shipped**: the "⚠️ Radio firmware ↔ ESPHome config matching" section in [README.md](../../README.md) covers the four-axis matching contract, the two failure modes, and the "edit YAML → build+OTA → then flash radio" change sequence. Mirror the warning in header comments of each `packages/roles/*.yaml` if/when those are written.

---

## 7. One-click radio flash from HA — feasibility

Not in v1, but worth designing so we don't paint ourselves into a corner. The operations that need to sequence:

1. **HA downloads firmware `.hex` from SMLIGHT** — trivial, `wget` or a shell command
2. **HA calls ESPHome service** `enter_bootloader_radio1` — ESPHome pulls DTR low, pulses RST, releases DTR. Well-defined for CC26xx.
3. **A flasher writes the firmware** — this is the hard part:
   - **For CC26xx**: `cc2538-bsl` (Python) speaks the TI serial bootloader protocol over TCP if we expose the UART as a TCP socket, or over local serial if flasher runs on the ESP32 itself.
   - **For EFR32**: Silicon Labs' Gecko Bootloader protocol via `commander` or `slzbfw`. Same TCP-or-local decision.
4. **HA calls ESPHome service** `exit_bootloader_radio1` — pulse RST, back to normal firmware.
5. **HA updates the "installed version" state.**

### Two viable placements for the flasher

**Option A — Flasher runs on the HA side**, talking over a temporary plaintext TCP socket that ESPHome opens for the duration of the flash:

- HA add-on (or shell_command) runs `cc2538-bsl`
- ESPHome exposes `bootloader_flash_radio1` service that opens `tcp://<esp>:7638` as a raw UART bridge for N seconds
- HA add-on connects, flashes, disconnects
- ESPHome closes the socket

**Pros**: reuses existing flasher tooling; ESPHome stays simple.  
**Cons**: opens a plaintext port temporarily (small window, LAN-only, auth-less); requires a HA add-on or Python shell_command; user needs Docker/add-on knowledge.

**Option B — Flasher runs on the ESP32**, embedded in ESPHome:

- Custom ESPHome component that implements cc2538-bsl protocol
- Service receives firmware bytes over encrypted API, writes to chip
- No temporary open port

**Pros**: purely encrypted API surface; simpler user setup (just click Install).  
**Cons**: significant C++ development; must be maintained; extends attack surface of ESPHome firmware; needs enough RAM to buffer the firmware image (CC2674P10 image is ~500KB — exceeds ESP32-S3 RAM; needs streaming or PSRAM).

### Recommendation for when we build this

**Option A**, packaged as an optional HACS add-on repo or a documented shell_command. Keeps ESPHome firmware minimal, doesn't expose an always-on plaintext port, and reuses well-tested flasher tooling. The temporary plaintext socket is a reasonable trade-off given: (a) short-lived, (b) LAN-only, (c) user-initiated, (d) same footgun-level as flashing via USB.

---

## 8. Package inventory to build

Groups the items above by ESPHome package file. Nothing implemented yet; this is a target inventory.

| Package | Purpose | Effort | Depends on |
|---|---|---|---|
| `packages/diagnostics/radio_probe_ext.yaml` + `radio{1,2,3}_probe.yaml` | Per-radio diagnostic sensors (chip, protocol, role, baud, hw_flow, channel) + `radio_probe:` instance publishing `installed_firmware` | Tiny YAML | hw_defs substitutions + `components/radio_probe/` (see [roadmap.md](roadmap.md) Step 1) |
| `components/radio_probe/` | Custom component: per-radio boot-time probe dispatched on `radio*_protocol`. See [roadmap.md](roadmap.md) Steps 1–2 for phased build (ZNP live for CC26xx and Spinel live for EFR32 Thread; EZSP + Z-Wave still stubbed, real probes ship in later v1.x point releases) | Medium (custom C++/Python) | UART bus (`setup_priority::BUS`, ~1000); runs at `on_boot: priority: 250` before `serial_proxy` attaches |
| `components/uart_hw_flow/` | Custom component that enables ESP-IDF UART HW flow control per UART | Small (~20 lines C++ + Python registration) | ESP-IDF HAL, must run after `App.setup()` |
| `packages/radio_control/radio1_restart_btn.yaml` | Button that pulses RST pin | Tiny | Existing hw_defs pin names |
| `packages/radio_control/radio2_restart_btn.yaml` | Same for radio 2 | Tiny | Existing hw_defs |
| `packages/radio_control/radio1_bootloader_btn.yaml` | DTR/RST bootloader-entry dance | Small | Existing hw_defs |
| `packages/radio_control/radio2_bootloader_btn.yaml` | Same for radio 2 | Small | Existing hw_defs |
| `packages/leds/led_disable_switch.yaml` | Global toggle to force all LEDs off | Tiny | Existing led packages |
| `packages/leds/led_night_mode.yaml` | Time-window LED disable | Small | led_disable_switch |
| `packages/diagnostics/radio_role_text.yaml` | Formats `radio*_role` → "coordinator/router/RCP/NCP/primary controller" | Tiny | radio_probe_ext |

Every package is opt-in via `!include`. Nothing here is required for a working build.

Effort: **half a day of YAML** for the wrapper packages; the two custom components (`radio_probe/` and `uart_hw_flow/`) are the main C++ work — see [roadmap.md](roadmap.md) for phased plan.

### HA-side snippets to ship

Under `docs/ha-integrations/`:

| File | Purpose |
|---|---|
| `smlight-firmware-update.yaml` | Combined REST catalog fetcher + per-radio `template: update:` entity with the six-field filter (chip, role, baud, hwFlow, channel, protocol-validated at compile time) |
| `README.md` | How to compose, recorder exclude rules, examples for MR4U + SLZB-06P10 |

One file rather than a REST-sensor file + template-entity file, because the two are tightly coupled (the template reads only from that REST sensor) and one paste is friendlier than two.

Note: no `channel` selector — it's a compile-time substitution in ESPHome and
is published as a text_sensor read by the update template.

### Blueprint (future v2)

Under `docs/ha-integrations/blueprints/`:

| File | Purpose |
|---|---|
| `flash-radio-firmware.yaml` | Blueprint that: downloads .hex, enters bootloader, runs flasher, exits bootloader, re-fires the boot probe |

Not shipping in v1. Design captured here so v2 doesn't require re-litigating architecture.

---

## 9. Open questions

None in this doc's scope. Residual work is tracked in [roadmap.md](roadmap.md) v1.x; the shipped decisions are collected in §10 below.

---

## 10. Summary of decisions

- **Static configuration principle** — every wire-level property lives in the YAML build config. No runtime mutation of protocol, role, baud, or channel.
- **Compile-time protocol + role selection** — `radio*_protocol` + `radio*_role` in `hw_defs` (with override in `mr4u-local.yaml`). No runtime switching. Users who want a change edit YAML *and take responsibility for flashing the matching radio firmware*.
- **Filter chain has six members** — chip_id, role→type, baud, hwFlow, channel; protocol is validated at compile time against the (chip, protocol, role) permutation matrix in [radio-probe-reference.md](radio-probe-reference.md) §6g but is not itself a catalog-filter axis. `hwFlow` is a real filter axis because MR4U hardware wires CTS/RTS and the `uart_hw_flow` component enables it on the ESP32 side when the firmware requires it.
- **HW flow custom component ships in v1** — ~20 lines of C++ calling ESP-IDF `uart_set_hw_flow_ctrl()`. Required for future MG24 Thread support; also correct-by-construction protection against manual-flash mismatches on MR4U.
- **No-match state is silent** — update entity shows "up-to-date" when filter returns empty. Simplest and clearest.
- **Baud is per-role**: Radio 1 (CC2674P10 coord) at 115200, Radio 2 (EFR32MG26 Thread) at 460800. Thread firmware is 460800-only industry-wide — no 115200 option exists. `uart*_hw_flow: false` for both current MR4U radios; matches SMLIGHT's published firmware.
- **`firmware_channel` is a substitution, not HA state** — `prod` / `dev` is a compile-time property in `hw_defs`, published to HA as a diagnostic. Value names match the SMLIGHT catalog's `prod: bool` field so the Jinja filter is a plain `selectattr('prod', 'eq', channel == 'prod')` with no translation table.
- **Live version probe is v1** — ZNP `SYS_VERSION` on boot for CC26xx (Radio 1) and Spinel `PROP_NCP_VERSION` for EFR32 Thread (Radio 2 when flashed with OT firmware), in-memory only, publishes `unknown` on failure. No hard-cached rev. EZSP + Z-Wave ship as `"unknown (… not implemented in v1)"` stubs, replaced in later v1.x point releases per [roadmap.md](roadmap.md).
- **One-click flash from HA is v2** — HA add-on running `cc2538-bsl` against a temporary raw TCP UART bridge ESPHome opens on demand. Not runtime-open, LAN-only, short-lived. See [roadmap.md](roadmap.md) v2 · flash chain.
- **v1 ships**: seven metadata sensors per radio, live ZNP + Spinel probes with stubs for EZSP/Z-Wave, HA REST sensor + template update entity, radio restart/bootloader buttons, LED control switches. See [roadmap.md](roadmap.md) for the phased Step 1–4 plan.

---

## 11. Feature parity and HA integration shape

Moved to [design.md](design.md):

- **§27** — SMLIGHT HA integration feature parity, package retention, BLE proxy caveat, post-v1 integration strategy. (Old §11.1–§11.4 landed as §27.1–§27.5, with the BLE caveat kept at §27.3 to preserve inbound cross-references.)
- **§28** — SLZB-OS web UI port/skip walkthrough and per-feature disposition. (Old §12 + §12.1 landed as §28.1 + §28.2.)
- **§26.6** — Implementation shape: what needs Python vs YAML vs a HA add-on, and which SLZB-OS features re-use v2's add-on infrastructure at near-zero cost. (Old §13.1–§13.5 landed as §26.6.1–§26.6.5.)
