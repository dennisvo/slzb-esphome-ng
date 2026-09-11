# v1 design considerations

Status: **design-level notes, no code yet.** Captures decisions and rationale
from the firmware-update investigation, plus feature-scope decisions for this
ESPHome fork against upstream SMLIGHT products. Serves as the paper trail for
future implementation work.

Scope of this doc:

- Radio-firmware management (\u00a71\u2013\u00a710): what update UX we ship, how role
  switching is handled, how firmware flashing might work later.
- Feature-parity assessment (\u00a711\u2013\u00a712): what we port from SMLIGHT's HA
  integration and SLZB-OS web UI, and what we intentionally skip.

Implementation-level details (snippets, exact filenames) live in
[v1-radio-firmware.md](v1-radio-firmware.md).

---

## 1. Scope decisions

### In scope for v1

- **Radio firmware update check** — HA `update` entity per radio, comparing installed version to SMLIGHT's public JSON catalog.
- **Radio metadata as ESPHome diagnostics** — chip, firmware type, baud, HW flow, firmware channel. Static per hardware revision, encoded in `hw_defs/**.yaml`.
- **Live installed-version probe** — ESPHome speaks ZNP `SYS_VERSION` to the radio at boot, publishes the real version as a text_sensor. In-memory only, no cached fallback. See v1-radio-firmware.md §6 for the frame layout.
- **HW flow control custom component** — small custom component that calls `uart_set_hw_flow_ctrl()` on the ESP-IDF UART HAL. Enables HW flow when the radio firmware requires it. See §5.
- **Feature-parity entities** from SMLIGHT's HA integration where they're cheap to add and useful — see §11 for the full port/skip table.

### In scope for v2 (or later, when someone wants to build it)

- **One-click radio flash from HA** — download firmware from SMLIGHT, put chip in bootloader, stream via serial, exit bootloader, report done. See §7.
- **Live probe for Thread (Spinel/HDLC-lite)** — same idea as ZNP but for EFR32MG26 Radio 2. Heavier framing; postponed.

### Out of scope indefinitely

- **Runtime role-switching** ("turn Radio 1 into a router with one click"). Discussed and rejected — see §6.
- **SLZB-OS-specific features**: web UI, embedded Zigbee Hub (Berry-scripted Z2M-alike), ZCN builder, embedded MQTT bridge, SLZB-OS backup/restore, VPN Wireguard, DDNS, AI Assistant, syslog. These are SLZB-OS product features, not radio-management features.
- **Radio-side sensors that require a persistent second protocol channel on the UART** — anything beyond the one-shot version probe done at boot before the proxy starts. SLZB-OS itself surfaces radio SoC die temperature (`zb_temp`, `zb_temp2` in `/ha_sensors`), so it *is* obtainable via a ZNP/Spinel MFG-INFO query; the reason we skip it isn't that it's impossible, it's that continuously polling the radio would break the `serial_proxy` dumb-pipe principle. Re-evaluate for v3+ if we ever add a scheduled break-glass query pattern.

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
Hub config page (which we're not porting; see §12). If we ever want to expose
"currently applied TX power" as a **diagnostic** we can add a v2-add-on-side ZNP
`SYS_GET_TX_POWER` query and publish it as a read-only text sensor. Never the
setter — that stays with the client.

---

## 4. Filter chain — matching a catalog entry to a device

An entry in SMLIGHT's `type=ZB` catalog is uniquely identified by six properties we care about. All six must match for us to consider it a valid upgrade candidate:

| Catalog field | Source of truth on our side | Notes |
|---|---|---|
| Top-level key (chip ID) | `radio*_smlight_id` (hw_defs) | e.g. `"70"` for CC2674P10 |
| `type` | `radio*_role` (hw_defs) | `coord`→`"0"`, `router`→`"1"`, `rcp`/`ncp`→`"2"`. See [v1-radio-firmware.md](v1-radio-firmware.md) §6g permutation matrix |
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
- Chip 68 Thread entries don't require HW flow (hwFlow absent in the catalog). Chips 65/67 (EFR32MG24) do — that's a future-device concern, not an MR4U one.
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

Existing `packages/buses/uarts/uart{1,2,3}_hwfc.yaml` files are byte-identical to their `_no_hwfc.yaml` siblings — the naming is aspirational. Once the custom component lands, collapse each pair into a single `uart{1,2,3}.yaml` that includes the custom component with `enabled: ${uartN_hw_flow}`. Housekeeping item in §9.

Existing `packages/buses/uarts/uart{1,2,3}_hwfc.yaml` files are
byte-identical to their `_no_hwfc.yaml` siblings \u2014 the naming is
aspirational. Housekeeping item in §9.

---

## 6. Role-switching — why we're not building it

Considered and rejected for v1. The rationale:

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

1. **They are responsible for flashing matching radio firmware before the next boot.** All four axes must line up: firmware protocol ↔ `radio*_protocol`, firmware role ↔ `radio*_role`, firmware baud ↔ `uart*_baud`, firmware `hwFlow` ↔ `uart*_hw_flow`.
2. If they build+OTA our fork without also reflashing the radio, the running image will attempt to speak (say) ZNP to a chip running Thread — no HA-side error, just silence. Same class of failure for baud or HW-flow mismatch.
3. The live-boot probe helps catch this: if the probe times out because the frame protocol or line settings don't match, the diagnostic `radio*_installed_firmware` sensor publishes `unknown` — a visible signal that ESPHome's declared configuration doesn't match the chip's real firmware.

**⚠️ README task when implementing v1**: add a prominent warning section to [README.md](../README.md) covering the config-firmware matching contract. Suggested content:

- Named "Radio firmware ↔ ESPHome config matching" or similar
- Lists the four axes (protocol/role/baud/hwFlow) that must agree
- Explains the two failure modes (protocol mismatch → silence; baud/hwFlow mismatch → garbled or blocked serial)
- Points to `docs/v1-design.md` §5 and §6 for the full story
- Includes the recommended change sequence: **edit YAML → build+OTA → THEN flash matching radio firmware**, never the reverse

Also mirror the warning in header comments of each `packages/roles/*.yaml` when those are written.

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
| `packages/diagnostics/radio_firmware_info.yaml` | 7 diagnostic sensors per radio: chip, smlight_id, protocol, role, baud, hw_flow, channel + the `radio_probe:` instance publishing `installed_firmware` | Tiny YAML | hw_defs substitutions + `components/radio_probe/` (see [roadmap.md](roadmap.md) Step 1) |
| `components/radio_probe/` | Custom component: per-radio boot-time probe dispatched on `radio*_protocol`. See [roadmap.md](roadmap.md) Steps 1–2 for phased build (ZNP live for CC26xx and Spinel live for EFR32 Thread; EZSP + Z-Wave still stubbed, real probes ship in later v1.x point releases) | Medium (custom C++/Python) | UART bus (`setup_priority::BUS`, ~1000); runs at `on_boot: priority: 250` before `serial_proxy` attaches |
| `components/uart_hw_flow/` | Custom component that enables ESP-IDF UART HW flow control per UART | Small (~20 lines C++ + Python registration) | ESP-IDF HAL, must run after `App.setup()` |
| `packages/radio_control/radio1_restart_btn.yaml` | Button that pulses RST pin | Tiny | Existing hw_defs pin names |
| `packages/radio_control/radio2_restart_btn.yaml` | Same for radio 2 | Tiny | Existing hw_defs |
| `packages/radio_control/radio1_bootloader_btn.yaml` | DTR/RST bootloader-entry dance | Small | Existing hw_defs |
| `packages/radio_control/radio2_bootloader_btn.yaml` | Same for radio 2 | Small | Existing hw_defs |
| `packages/leds/led_disable_switch.yaml` | Global toggle to force all LEDs off | Tiny | Existing led packages |
| `packages/leds/led_night_mode.yaml` | Time-window LED disable | Small | led_disable_switch |
| `packages/diagnostics/radio_role_text.yaml` | Formats `radio*_role` → "coordinator/router/RCP/NCP/primary controller" | Tiny | radio_firmware_info |

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

Resolved:

- **Naming** — sensor is `radio*_installed_firmware`; the underlying value is the catalog's `rev` (YYYYMMDD string). Apply consistently.
- **No-match behaviour** — silent (§5).
- **Where the installed-firmware value lives** — ESPHome live probe at boot, in-memory `text_sensor` only, no hard-cached value. Publishes `unknown` on probe failure or timeout (§3, §6, §8).
- **`firmware_channel`** — compile-time substitution in ESPHome, not HA runtime state. Values `prod` / `dev` matching the SMLIGHT catalog's `prod: bool` field verbatim (no translation layer inside our Jinja).
- **Two-dim role encoding** — `radio*_protocol` + `radio*_role` (see [v1-radio-firmware.md](v1-radio-firmware.md) §6g). Single-dim `radio*_firmware_type` is retired.
- **HW flow custom component** — ships in v1 (see §5).
- **HA snippet packaging** — one combined file `docs/ha-integrations/smlight-firmware-update.yaml`, not two (§8).

Still open:

1. **`prod` field completeness in the ZB catalog**: sanity-check that every ZB entry carries `prod: true|false` (not absent, not `dev: true` only). If some entries lack `prod`, the filter needs a `default(false)`-style fallback.

2. **First device to target**: MR4U (this repo's primary user's device). Adding SLZB-06P10 as a second target after MR4U ships would validate that the (chip, role, baud, channel, hwFlow) filter design generalises.

3. **HACS or vanilla HA `packages:`?** The snippets in `docs/ha-integrations/` can be pasted into `configuration.yaml`, or bundled as a HA packages directory the user drops in. HACS custom repository would be nicer discoverability but adds installation burden. Recommend: vanilla, with clear copy-paste instructions.

4. **UART package rename**: `packages/buses/uarts/uart1_hwfc.yaml` now carries the `uart_hw_flow:` block from §5, but its `uart1_no_hwfc.yaml` sibling is still a byte-identical dead file. Housekeeping — collapse to one file (e.g. `uart1.yaml`) next time we're touching this area.

5. **README warning on firmware ↔ config matching**: when v1 lands, [README.md](../README.md) needs a prominent section explaining that radio firmware protocol/role/baud/hwFlow must match the ESPHome substitutions. See §6 "User responsibility" for the shape.

---

## 10. Summary of decisions

- **Static configuration principle** — every wire-level property lives in the YAML build config. No runtime mutation of protocol, role, baud, or channel.
- **Compile-time protocol + role selection** — `radio*_protocol` + `radio*_role` in `hw_defs` (with override in `mr4u-local.yaml`). No runtime switching. Users who want a change edit YAML *and take responsibility for flashing the matching radio firmware*.
- **Filter chain has six members** — chip_id, role→type, baud, hwFlow, channel; protocol is validated at compile time against the (chip, protocol, role) permutation matrix in [v1-radio-firmware.md](v1-radio-firmware.md) §6g but is not itself a catalog-filter axis. `hwFlow` is a real filter axis because MR4U hardware wires CTS/RTS and the `uart_hw_flow` component enables it on the ESP32 side when the firmware requires it.
- **HW flow custom component ships in v1** — ~20 lines of C++ calling ESP-IDF `uart_set_hw_flow_ctrl()`. Required for future MG24 Thread support; also correct-by-construction protection against manual-flash mismatches on MR4U.
- **No-match state is silent** — update entity shows "up-to-date" when filter returns empty. Simplest and clearest.
- **Baud is per-role**: Radio 1 (CC2674P10 coord) at 115200, Radio 2 (EFR32MG26 Thread) at 460800. Thread firmware is 460800-only industry-wide — no 115200 option exists. `uart*_hw_flow: false` for both current MR4U radios; matches SMLIGHT's published firmware.
- **`firmware_channel` is a substitution, not HA state** — `prod` / `dev` is a compile-time property in `hw_defs`, published to HA as a diagnostic. Value names match the SMLIGHT catalog's `prod: bool` field so the Jinja filter is a plain `selectattr('prod', 'eq', channel == 'prod')` with no translation table.
- **Live version probe is v1** — ZNP `SYS_VERSION` on boot for CC26xx (Radio 1) and Spinel `PROP_NCP_VERSION` for EFR32 Thread (Radio 2 when flashed with OT firmware), in-memory only, publishes `unknown` on failure. No hard-cached rev. EZSP + Z-Wave ship as `"unknown (… not implemented in v1)"` stubs, replaced in later v1.x point releases per [roadmap.md](roadmap.md).
- **One-click flash from HA is v2** — HA add-on running `cc2538-bsl` against a temporary raw TCP UART bridge ESPHome opens on demand. Not runtime-open, LAN-only, short-lived. See [roadmap.md](roadmap.md) v2 · flash chain.
- **v1 ships**: seven metadata sensors per radio, live ZNP + Spinel probes with stubs for EZSP/Z-Wave, HA REST sensor + template update entity, radio restart/bootloader buttons, LED control switches. See [roadmap.md](roadmap.md) for the phased Step 1–4 plan.

## 11. Feature parity — SMLIGHT HA integration

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

### 11.1 Retention via SMLIGHT ESPHome packages

SMLIGHT's `slzb-esphome` repo (which we fork) already contains ESPHome-native implementations of every feature the `smlight` integration currently wraps. By keeping SMLIGHT's packages and only removing the network-exposure ones, we retain the feature set at essentially zero extra cost:

| Feature (via `smlight` integration today) | Keep from `slzb-esphome` |
|---|---|
| Radio reset buttons | `packages/diagnostics/` |
| Buzzer + RTTTL playback | `packages/buzzer/` |
| IR TX / RX + code library | `packages/ir/`, `libraries/ir/codes/` |
| WS2812 LED effects + presets | `packages/ws2812/`, `packages/leds/` |
| BLE proxy | `packages/bluetooth/` (see §11.3) |
| Diagnostics sensors, firmware version | `packages/diagnostics/` |
| Firmware updates | ESPHome OTA |
| Wi-Fi transport | `packages/wifi/` |
| Ethernet transport | `packages/ethernet/` |

**Remove:** `packages/stream_servers/`, any web-UI packages, SLZB-OS-only server features.

### 11.2 HA-side integration: two-stage strategy

**v1 — stock ESPHome integration, no HA-side code.**

ESPHome exposes entities transport-defined: any entity declared in the firmware YAML surfaces in HA automatically through the standard ESPHome integration. That means:

- BLE proxy is picked up by HA's Bluetooth integration as a proxy (first-class ESPHome feature).
- Buzzer RTTTL surfaces as `esphome.<device>_rtttl_input_set` — same call pattern SMLIGHT's README already documents.
- IR TX/RX, WS2812 effects, radio reset buttons, sensors — all appear as normal HA entities.

Functional coverage matches the SLZB-OS path. What is lost is polish: the device card shows "ESPHome mr4u" rather than a branded "SMLIGHT SLZB-MR4U" card, and there is no MR4U-specific setup wizard. SLZB-OS-only server features (SLZB-OS scripting, cloud, proprietary UI) are gone by design.

**Post-v1 — extend the existing `smlight` integration, do not fork.**

Same cooperation path as [design.md](design.md) §26.3 Path (a). Propose adding an ESPHome-firmware backend to the existing `smlight` integration:

- Add a zeroconf matcher for `_esphomelib._tcp.local.` scoped to our device naming pattern (e.g. `slzb-mr4u-*`).
- On discovery, probe: SLZB-OS HTTP endpoint present? → use `pysmlight` backend. Not present? → adopt the device via the ESPHome integration and surface a curated subset of its entities in the `smlight` device card.
- Share the config flow, device registry entry, diagnostics, and update-entity plumbing across both backends.
- Auth: SLZB-OS backend uses its own token via `pysmlight`; ESPHome backend delegates to the ESPHome integration's PSK handling. No new secret storage.

This is a **larger** PR than the USB matcher work (touches coordinator, config flow, probably depends on `esphome` integration primitives), but it preserves the "SMLIGHT device" mental model regardless of which firmware the user runs. Discuss with `@tl-sl` before starting.

**Do not** write a standalone new integration. A parallel `smlight_esphome` domain duplicates the config flow, discovery, and coordinator layers and drifts out of sync over time.

### 11.3 Bluetooth proxy caveat

BLE proxy on the ESP32-S3 shares CPU and radio time with the UART transports. SMLIGHT's own README already warns that the on-board microphone "may cause instability or packet loss" on concurrent Zigbee/Thread/Z-Wave UART-to-Ethernet operation. The same concern applies to BLE proxy.

Note: the SLZB-OS BLE settings page itself links to "ESPHome BT proxy firmware" as the *alternative* to its built-in BLE feature — direct confirmation from SMLIGHT that ESPHome is the right stack for BLE on this hardware.

Rule for our firmware:

- **Retain** the BLE proxy capability in the YAML (from `packages/bluetooth/`).
- **Disable by default.** Ship v1 with BLE proxy off so it does not affect the primary Zigbee/Thread reliability testing.
- Expose it as an opt-in switch/config so users can enable and validate against their own workload.
- Do not enable BLE proxy during Phase 3 / Phase 5 soak tests — those must reflect the default configuration.

### 11.4 Summary

- Feature parity: **retain** by reusing SMLIGHT's ESPHome packages.
- v1 HA integration: **none required** — stock ESPHome integration surfaces everything.
- Post-v1 polish: **extend the existing `smlight` integration**, don't fork it.
- BLE proxy: capability retained, **off by default**.

## 12. Feature parity — SLZB-OS web UI

Cross-check against the 25 sections of the stock SLZB-OS web UI. Groups pages by whether their function is portable to our ESPHome + HA model.

| SLZB-OS section | Function | Decision | Notes |
|---|---|---|---|
| §1 Home / dashboard | Status overview | Port (v1) | HA dashboard replaces it; entities from §11 provide all data |
| §2 Mode (per-radio role picker) | Runtime role change | **Skip** | See \u00a76. Compile-time only. `packages/roles/*.yaml` includes if we ever want a shortcut |
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
| §18 Update host firmware | Check + install | Port (partial) | ESPHome OTA already handles install; catalog check for host is skipped (see \u00a711) |
| §19 Update Zigpy-znp / ZHA | Radio protocol lib | **Skip** | HA-side; not ESPHome's concern |
| §20 Update SLZB-OS | Host OS | **Skip** | Replaced by ESPHome OTA |
| §21 Firmware update (radio) | Check + flash per radio | Port (v1 check, v2 flash) | The central feature of this design |
| §22 Buzzer test | Play tone | Port (optional) | ESPHome `rtttl:` action button |
| §23 Buttons test | Hardware self-test | **Skip** | One-off diagnostic; not worth automating |
| §24 About | Device info | Port (v1) | ESPHome diagnostic sensors already publish this |
| §25 API reference | Docs | **Skip** | ESPHome Native API is a different API; docs live in this repo |

**Summary**: 14 sections port cleanly, 11 skip for reasons that boil down to "wrong layer for this fork" (auth, VPN, backup, host-OS updates) or "compile-time-only" (role change). The port/skip split validates that the ESPHome + HA-Native-API model covers the useful surface without needing a custom integration.

### 12.1 Feature disposition — what we keep vs skip vs retain-off

A sweep of the stock SLZB-OS web UI on the MR4U surfaces several features beyond Zigbee/Thread transport. Each is dispositioned explicitly so future contributors know what was considered and why it isn't in v1.

| SLZB-OS feature | v1 disposition | Rationale |
|---|---|---|
| Zigbee coordinator on either radio | Keep | Primary use case; either UART can carry it. |
| Thread to remote OTBR | Keep | Our primary Thread path (see [integration-recipes.md](integration-recipes.md) §3). |
| Thread + on-device OTBR (beta) | Non-goal | Border-router state/routing belong in a supervised add-on, not on the MCU. |
| Matter-over-Thread mode on the gateway | Non-goal | HA already brokers Matter via OTBR. |
| Zigbee Hub (beta) | Non-goal | We host no Zigbee application layer on-device. |
| USB-to-Ethernet passthrough (SLZB-OS :8638 for external USB dongles) | Non-goal | Same insecure raw-TCP category as :6638/:7638. If needed later, expose via `serial_proxy` over encrypted Native API. |
| VPN | Non-goal | Network reachability is the operator's problem; no cloud/proprietary dependency. |
| Cloud firmware-update check | Non-goal | Manual, encrypted radio-firmware flow is future work (see [roadmap.md](roadmap.md) v2). |
| BLE + BLE proxy | Retain, off by default (§11.3) | SLZB-OS itself links to "ESPHome BT proxy firmware" as the alternative. |
| BLE scan interval / window tuning | Expose as ESPHome numbers | Trivial from `packages/bluetooth/`. |
| Radio reset / bootloader entry buttons | Keep | Native API actions, authenticated. See [design.md](design.md) §15. |
| IEEE address read/write | Keep | Essential for coordinator migration without re-pairing. Ownership rule applies. See [design.md](design.md) §15, §29. |
| Zigbee channel energy scan | Keep | Clean-channel diagnostic. Ownership rule applies. |
| Buzzer + RTTTL, IR TX/RX, WS2812 effects | Keep (§11.1) | Reused SMLIGHT ESPHome packages. |
| Dashboard: SoC temperature, uptime, radio FW versions, connection status | Expose as ESPHome sensors | Free via ESPHome; surfaces to HA automatically. |
| Concurrent USB + Wi-Fi/Ethernet + web server | Not exposed | Hardware supports it; firmware keeps transports mutex at build time for UX/testing simplicity. See [design.md](design.md) §25. |
| SLZB-OS web UI, scripting, proprietary HTTP API | Removed | Attack-surface reduction is the whole point of this project. See [design.md](design.md) §3. |
| ADVANCED socket options (Zigbee Socket packet processing, multi-threaded socket, Multi-Radio Queue Control) | Not applicable | These are workarounds for raw-TCP-socket semantics, which we don't expose. |

Bold summary: every operator-facing capability is either **kept**, **retained-but-off**, or **surfaced automatically** through ESPHome. Everything SLZB-OS-specific (on-device OTBR, Matter endpoint, Zigbee Hub, VPN, cloud FW pull, raw-TCP USB passthrough, proprietary UI) is deliberately out and stays out.

---

## 13. HA integration scope: what needs Python, what stays in YAML

Where does "a HA-side integration" become necessary vs optional? The answer changes between v1 and v2, and this section makes that boundary explicit so we don't
accidentally build more than we need or defer things that would in fact become
cheap once we've committed to v2.

### 13.1 v1 needs no Python integration

Every entity in §11's port list is either:

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

**No `custom_components/` directory, no `manifest.json`, no config flow.** It's all
YAML plus (optionally) an add-on the user installs from a documented URL.

### 13.2 What v2 actually needs

v2's non-negotiable is **one-click Zigbee-radio firmware flash from HA**. The
ESP32 cannot host the flasher binaries (silabs-firmware-flasher, cc2538-bsl.py,
Python OT/Spinel tooling), so something HA-side must:

1. React to a HA event or service call.
2. Open TCP to a temporary raw-UART bridge port that ESPHome opens on demand
   (see §7 Option A).
3. Run the appropriate flasher against `socket://<esphome-ip>:<bridge-port>`.
4. Publish progress + result back into HA.

The lowest-effort shape that satisfies all four is a **Home Assistant add-on**
(Docker container in the HA supervisor). Options in increasing complexity:

| Shape | Effort | User install path | Trade-offs |
|---|---|---|---|
| Documented `shell_command:` invoking a flasher binary the user installs manually | Very low (docs only) | Manual copy of a Python script | Fragile; no HA event pipeline; Windows/macOS HA users left out |
| **HA add-on published to a HACS repo** ← preferred | Medium | Add repo URL, install add-on | Runs in HA supervisor; can publish events; auto-updates; still no `custom_components/` needed |
| Full `custom_components/slzb_mr4u/` with add-on backend | High | HACS integration install | Nice service registration + Developer-Tools UX; more code to maintain |

We can defer choosing between "add-on only" and "add-on + custom_component" until
v2.0 ships. Start with the add-on, add the thin custom_component wrapper only if
users complain about UX.

### 13.3 Which SLZB-OS features re-use v2's infrastructure at near-zero cost

Once we're paying for an add-on with Python + a ZNP client + a bridge to ESPHome,
several previously-skipped items become cheap. The framework: **if it needs a
Python-side ZNP/Spinel session, it belongs in v2. If it needs a persistent Zigbee
client role, it belongs in Z2M/ZHA. If it needs a runtime on the device, it stays
skipped.**

| SLZB-OS feature | v1 decision | v2 add-on decision | Reasoning |
|---|---|---|---|
| One-click radio flash | Skip | **v2.0 core** | The reason v2 exists |
| Post-flash re-probe | N/A | **v2.0 core** | Also the escape hatch that makes a `zbVer.txt`-style cache safe later (§3 sidebar) |
| Flash progress events | Skip | **v2.0 nice-to-have** | Add-on emits `slzb.flash_progress` events; replaces SLZB-OS SSE `/events` pattern without needing SSE on ESP32 |
| Zigbee network backup (nwk key + PAN + device table export) | Skip | **v2.1 candidate** | Same ZNP session dumps to `/config/backups/`; parallel to HA snapshots, valuable for network-key rotation and disaster recovery |
| IEEE MAC read + migrate helper | Skip | **v2.2 candidate** | ZNP CMD 12/14 read, CMD 11 write. Real UX win for "I swapped a CC26xx" recovery |
| Radio SoC die temperature | v3+ | **v2.3 candidate** | ZNP `SYS_GET_MFG_INFO` (or Silabs equivalent) polled on the add-on's existing schedule. Publishes as HA sensor. Was v3+ under v1's constraints; v2 makes it v2.3. |
| Zigbee energy scan | Skip | **v2.4 candidate** | ZNP CMD 5 equivalent. Nice channel-picker visualization on the HA side. |
| Diagnose "why won't my network start" (RSSI/link/permit-join snapshot) | Skip | **v2.4 candidate** | Piggy-backs on energy scan |
| Radio TX power *read* (diagnostic) | Skip | **v2.4 candidate** | ZNP `SYS_GET_TX_POWER` on schedule. Setter stays with the client (§3 sidebar). |
| WireGuard, DDNS | Skip | **Still skip** | Wrong layer regardless of v2 |
| Filesystem-over-HTTP | Skip | **Still skip** | Security anti-feature (see security-findings doc) |
| AI Assistant (Claude proxy) | Skip | **Still skip** | Security posture; not building agent-with-device-control |
| BE apps, Berry scripts, script integrations catalog | Skip | **Still skip** | Architectural — no on-device app runtime |
| Zigbee Hub (in-device Z2M-alike) | Skip | **Still skip** | Z2M/ZHA already do this in HA |
| USB gadget mode, CAN, IR, RF | Skip / per-device | **Still skip** | Hardware or per-device build, not v2 add-on scope |
| WiFi scan UI | Skip | **Still skip** | Native to HA's ESPHome integration |
| Runtime role/coord-mode switching | Skip | **Still skip** | Violates §2; compile-time only |
| Web auth, users, web UI | Skip | **Still skip** | Native ESPHome `web_server: auth:` |

### 13.4 Scope-creep footgun

If v2 grows from "one-click flash" to "flash + backup + migrate + temp poll +
energy scan", the add-on becomes A Whole Thing: HACS repo, release cadence,
issue tracker, documentation site, breaking-change management. That's the actual
cost, not the code.

Suggested phasing:

- **v2.0**: add-on ships with one-click flash + post-flash re-probe + progress events. Prove the architecture works.
- **v2.1**: ZB network backup export.
- **v2.2**: IEEE MAC read + migrate helper.
- **v2.3**: periodic radio SoC temp diagnostic.
- **v2.4**: energy scan + link diagnostics + TX power read.

Each increment reuses infrastructure the previous ones already built. Users who
only want v1 keep paying nothing.

### 13.5 The v1/v2 boundary in one line

**The boundary isn't "ESPHome-side vs HA-side." It's "things that only need the
ESP32 (v1) vs things that need Python-side ZNP/Spinel talking to the radio (v2)."**

SLZB-OS collapses that boundary by putting a Berry runtime on the device. We
keep the boundary and put Python on the HA side. That's the entire architectural
disagreement between the two forks, distilled.

