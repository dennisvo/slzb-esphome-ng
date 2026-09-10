# v1 radio firmware version reporting — design notes

Status: **design draft**, not yet implemented.
Scope: how users of this fork can see "your Zigbee/Thread radio firmware is out of date" without reflashing to SLZB-OS.

---

## 1. Background — what SLZB-OS does

SLZB-OS's own "Firmware update" page shows, per radio, the currently installed
firmware version, a "Check for updates" button, and a "Flash" button. When we
replace SLZB-OS with this ESPHome fork we lose that UI (and Home Assistant's
official `smlight` integration, which just proxies SLZB-OS's local HTTP API via
`pysmlight`).

**Where does that "latest available" data actually come from?** We inspected
the SLZB-OS web UI in Chrome DevTools (Network tab) and clicked
"Check for updates". The browser fires two `fetch()` calls straight from the
JavaScript UI to SMLIGHT's public update server — the device itself does no
polling.

The endpoints:

```
GET https://updates.smlight.tech/services/api/slzb-06x-ota.php?type=ZB&format=slzb
GET https://updates.smlight.tech/services/api/slzb-06x-ota.php?type=ESPs3
```

Both are public HTTPS, no auth, no token, no user-agent gating. The response
is JSON. The firmware binaries referenced by `.link` fields are also public
(also on `updates.smlight.tech`) and unauthenticated.

Despite the URL name, `slzb-06x-ota.php` is the **shared catalog for the entire
SLZB range**, including the MR series.

---

## 2. The SMLIGHT update catalog

### Endpoints

| URL | Purpose |
|-----|---------|
| `…/slzb-06x-ota.php?type=ZB&format=slzb` | Radio-chip firmware catalog (Zigbee coordinator, Zigbee router, OpenThread RCP) |
| `…/slzb-06x-ota.php?type=ESPs3` | ESP32-S3 host firmware (SLZB-OS core, U-series build) |

The `type` parameter is strictly validated. `type=Thread`, `type=OT`,
`type=EFR`, `type=EFR32`, and adding `board=`/`device=`/`model=` all return
HTTP 400. **Thread firmware isn't a separate query** — it's inside the `type=ZB`
response as `type: "2"`.

### Schema (`type=ZB&format=slzb`)

Top-level object is keyed by a SMLIGHT-internal **chip/board ID** (numeric
string). Each value is an array of firmware entries:

```json
{
  "70": [
    {
      "type": "0",            // 0 = Coordinator (ZNP), 1 = Router (ZR), 2 = Thread (OT-RCP)
      "baud": 115200,
      "prod": true,           // true = stable release, false = dev/beta
      "rev": "20251212",      // build timestamp, sortable string
      "notes": "…HTML release notes…",
      "link": "https://updates.smlight.tech/firmware/smhub/radio/essential/cc26x/cc2674p10/znp-SLZB-06P10-20250325_signed.hex"
    }
  ],
  "68": [ … ],
  ...
}
```

Some entries have additional fields (`hwFlow: true`, `ver: "v3.2.4"`,
`dev: true` — appears alongside `prod` in the ESPs3 feed).

### Chip catalog (what we've mapped so far)

| ID | Chip | Path fragment | Notes |
|----|------|---------------|-------|
| `70` | CC2674P10 | `essential/cc26x/cc2674p10/` | MR4U Radio 1 (UART1); SLZB-06P10 |
| `69` | CC1352P7 | `smhub-ccp7/` | SLZB-06P7 |
| `68` | EFR32MG26 | `smhub-mg26/` | MR4U Radio 2 (UART2); SLZB-06Mg26U |
| `91` | EFR32MG24 (Zigbee Bridge SDK v9.1) | `zrel/siMg24_zigbee_bridge/` | ZRel bridge builds |
| `67` | EFR32MG24 | `slzb06Mg24/`, `slzb07Mg24/` | SLZB-06Mg24, SLZB-07Mg24 |
| `21`, `23`, `0`–`5`, `12`, `13`, `16`–`18`, `65`, `66` | (varies) | (mixed) | Older / other product-line SKUs |

For MR4U specifically we care about `"70"` (CC2674P10) and `"68"` (EFR32MG26).

### Schema (`type=ESPs3`)

Same shape but keyed differently — this is the SLZB-OS host firmware catalog.
For anyone running this ESPHome fork, **this feed is irrelevant**: we don't
run SLZB-OS on the ESP32-S3 anymore, so its "latest" version doesn't apply to
us. Users updating this fork pull our own YAML/OTA releases from
[dennisvo/slzb-esphome-ng](https://github.com/dennisvo/slzb-esphome-ng).

---

## 3. Design — exposing device metadata from ESPHome

To build a HA update entity we need three things:

1. **Which radio chips this hardware has** → static per-`hw_defs` file
2. **What protocol and role each radio speaks** → mostly static per hw_defs (this fork ships coord + thread), user-overridable if they change it (see §10 configuration surface)
3. **What version is currently installed** → live-probed at boot (see §6). If the probe fails or the protocol isn't implemented, publish `"unknown"` — no user-declared fallback (see §10 rationale)

### Proposed substitutions (per hw_defs file) — two-dimensional

Additive — doesn't break existing packages. Two dimensions matter and cannot
be collapsed into one:

- **`radioN_protocol`** — determines which wire language the boot-time probe
  speaks (`znp` | `spinel` | `ezsp` | `zwave` | `none`).
- **`radioN_role`** — informational + used for SMLIGHT catalog matching
  (`coord` | `router` | `rcp` | `ncp` | `primary_ctrl`).

They are orthogonal: the same chip family can run different protocols
depending on which firmware image is flashed (see §6g permutation matrix).

```yaml
# hw_defs/mrxu/mr4u_r1_73.yaml
substitutions:
  # Radio 1 (UART1) — Zigbee coordinator on TI CC2674P10
  radio1_chip: cc2674p10
  radio1_smlight_id: "70"          # matches SMLIGHT JSON catalog key
  radio1_protocol: znp             # znp | spinel | ezsp | zwave | none
  radio1_role: coord               # coord | router | rcp | ncp | primary_ctrl
  radio1_firmware_channel: dev     # dev | prod — for HA "latest available" lookup

  # Radio 2 (UART2) — OpenThread RCP on Silabs EFR32MG26
  radio2_chip: efr32mg26
  radio2_smlight_id: "68"
  radio2_protocol: spinel
  radio2_role: rcp
  radio2_firmware_channel: dev
```

Same pattern for other hardware:

| hw_defs file | Radio | `chip` | `smlight_id` | `protocol` | `role` |
|---|---|---|---|---|---|
| `mrxu/mr4u_r1_73.yaml` | Radio 1 | `cc2674p10` | `70` | `znp` | `coord` |
| `mrxu/mr4u_r1_73.yaml` | Radio 2 | `efr32mg26` | `68` | `spinel` | `rcp` |
| `06xu/r1_73.yaml` | Radio | *(per stocked variant)* | `70` / `68` / `67` / `91` | *(per variant)* | `coord` |
| `ultima/r1_04.yaml` | Radio 1 | `cc2674p10` | `70` | `znp` | `coord` |
| `ultima/r1_04.yaml` | Radio 2 | `efr32mg26` | `68` | `spinel` | `rcp` |
| `ultima/r1_04.yaml` | Radio 3 | `zw800` | `""` | `zwave` | `primary_ctrl` |
| `slw09u/r1_01.yaml` | *(no radio)* | — | — | `none` | — |

The nice property: **every device family has a small, closed set of (chip,
protocol, role) triples.** Once encoded, the same HA snippet works for all of
them. §6g documents which triples are *valid* — invalid combinations (`znp`
+ `rcp`, `spinel` + `coord`, etc.) are rejected at build time.

**Explicit non-substitution**: there is no `radioN_installed_rev` any more.
The probe returns a real value or the sensor publishes `"unknown"`. Rationale:
users flash radios out-of-band and forget to bump the substitution → the
declared value becomes actively misleading. Honest ignorance beats a stale
lie. See §10 for the alternative (HA `select` entities as post-adoption
override).

### Proposed package (`packages/diagnostics/radio_firmware_info.yaml`)

Sketch — six `text_sensor`s that publish the metadata over Native API so HA can
consume them:

```yaml
text_sensor:
  - platform: template
    name: "Radio 1 chip"
    id: radio1_chip_sensor
    entity_category: diagnostic
    lambda: 'return {"${radio1_chip}"};'

  - platform: template
    name: "Radio 1 SMLIGHT id"
    id: radio1_smlight_id_sensor
    entity_category: diagnostic
    lambda: 'return {"${radio1_smlight_id}"};'

  - platform: template
    name: "Radio 1 firmware type"
    id: radio1_firmware_type_sensor
    entity_category: diagnostic
    lambda: 'return {"${radio1_firmware_type}"};'

  - platform: template
    name: "Radio 1 installed firmware"
    id: radio1_installed_rev_sensor
    entity_category: diagnostic
    lambda: 'return {"${radio1_installed_rev}"};'

  # …same block for Radio 2 (guard with $$radio2_chip presence)…
```

Cost on the device side: **zero runtime work**. These are compile-time
substitutions materialised as template sensors published once at boot. No UART
traffic, no radio interaction, no race with `serial_proxy`.

---

## 4. Ready-to-paste HA snippet — dynamic across SMLIGHT devices

The `sensor.<device>_radio_1_smlight_id` above is the whole trick: HA doesn't
need to know it's talking to an MR4U vs an SLZB-06P10. It reads that ID from
the device and uses it to select the right entry from the JSON catalog.

Users drop this into `configuration.yaml` (or a package file). One block
handles any number of SMLIGHT devices running this fork.

```yaml
# HA configuration.yaml
rest:
  - resource: https://updates.smlight.tech/services/api/slzb-06x-ota.php?type=ZB&format=slzb
    scan_interval: 86400        # once a day
    timeout: 15
    sensor:
      - name: SMLIGHT firmware catalog raw
        unique_id: smlight_fw_catalog_raw
        value_template: "ok"
        json_attributes_path: "$"
        json_attributes: ["0","1","2","3","4","5","12","13","16","17","18","21","23","65","66","67","68","69","70","91"]

template:
  # One update entity per (device, radio) pair. Copy the block, change the
  # slugs / entity references. Nothing else.
  - update:
      - name: "MR4U Zigbee radio firmware"
        unique_id: mr4u_radio1_firmware_update
        # Installed version — read from the ESPHome device itself
        installed_version: >
          {{ states('sensor.mr4u_radio_1_installed_firmware') }}
        # Latest available — look up (chip_id, firmware_type) in the catalog
        latest_version: >
          {% set chip = states('sensor.mr4u_radio_1_smlight_id') %}
          {% set ftype = states('sensor.mr4u_radio_1_firmware_type') %}
          {% set entries = state_attr('sensor.smlight_firmware_catalog_raw', chip) or [] %}
          {% set match  = entries | selectattr('type', 'eq', ftype)
                                  | selectattr('prod', 'eq', true) | list %}
          {{ match[0].rev if match else states('sensor.mr4u_radio_1_installed_firmware') }}
        release_summary: >
          {% set chip = states('sensor.mr4u_radio_1_smlight_id') %}
          {% set ftype = states('sensor.mr4u_radio_1_firmware_type') %}
          {% set entries = state_attr('sensor.smlight_firmware_catalog_raw', chip) or [] %}
          {% set match  = entries | selectattr('type', 'eq', ftype)
                                  | selectattr('prod', 'eq', true) | list %}
          {{ match[0].notes if match else "" }}
        release_url: >
          {% set chip = states('sensor.mr4u_radio_1_smlight_id') %}
          {% set ftype = states('sensor.mr4u_radio_1_firmware_type') %}
          {% set entries = state_attr('sensor.smlight_firmware_catalog_raw', chip) or [] %}
          {% set match  = entries | selectattr('type', 'eq', ftype)
                                  | selectattr('prod', 'eq', true) | list %}
          {{ match[0].link if match else "" }}
        entity_picture: >
          https://smlight.tech/wp-content/uploads/2024/06/logo.png
```

### Why this scales cleanly

- A user with an MR4U and an SLZB-06P10 uses **the same template** — each device reports its own `radio1_smlight_id`, so the lookup naturally picks the right catalog entry.
- Adding a new hardware revision to this fork = one new `hw_defs/**.yaml` with the four `radio1_*` / `radio2_*` substitutions. No HA config changes required.
- A hypothetical user who flashes a *router* firmware (`type: "1"`) onto the CC2674P10 just changes `radio1_firmware_type: "1"` in their local YAML — the HA entity re-derives.

### Where the JSON parsing lives

Home Assistant's `rest:` sensor with `json_attributes` doesn't natively unpack
`$.70[?(@.type=='0' && @.prod==true)]` style JSONPath filters, so we do the
picking client-side in the template. The `rest:` sensor just dumps the whole
JSON into attributes; the `template: update:` block filters at render time.

**Trade-off**: the `sensor.smlight_firmware_catalog_raw` entity gets a large
attribute payload (~15 KB). That's fine functionally but bloats the recorder
DB. Mitigation: exclude it from recorder via `recorder: exclude: entity_globs:`.
The snippet should include that guidance.

---

## 5. Suggested shipping form — **v1 ships live probing**

Scope revision (2026-09-10): **v1 ships the live-probe mechanism, not just
substitutions.** Radio flashing itself remains v2 (needs the HA add-on
described in the design doc §13). v1 is read-only: report installed version
per-radio; HA compares against SMLIGHT catalog.

Concrete v1 shape:

- **Substitutions in `hw_defs/**`** (backward-compatible, one block per radio) — see §3 for the full shape. Six substitutions per radio:
  - `radioN_chip` — chip family label (`cc2674p10`, `cc1352p7`, `efr32mg26`, `efr32mg24`, `zw800`)
  - `radioN_smlight_id` — SMLIGHT catalog key (`70`, `69`, `68`, `67`, `91`, or `""` if not in Zigbee catalog)
  - `radioN_protocol` — wire protocol the probe speaks (`znp` | `spinel` | `ezsp` | `zwave` | `none`)
  - `radioN_role` — firmware role for catalog matching (`coord` | `router` | `rcp` | `ncp` | `primary_ctrl`)
  - `radioN_firmware_channel` — `prod` or `dev` (defaults to `dev` per §7.1 evidence; matches the SMLIGHT catalog's `prod: bool` field verbatim, so the Jinja filter is a plain `selectattr('prod', 'eq', channel == 'prod')` with no translation layer). Used only for "latest available" HA lookup; currently-installed channel is auto-derived from the live-probed `rev` matching a catalog entry (§7.5).
  - `radioN_uart_baud` — already declared as `uartN_baud` for the UART; republished as a diagnostic sensor so HA can match against the catalog's `baud` field for builds that ship in multiple baud variants.
  - **No `radioN_installed_rev`.** The probe returns a real value or the sensor publishes `"unknown"`. See §3 and §10.

- **New package `packages/diagnostics/radio_firmware_info.yaml`**:
  - Publishes five text_sensors + one number-sensor per radio: `chip`, `smlight_id`, `protocol`, `role`, `firmware_channel`, `uart_baud`.
  - Publishes `radioN_installed_firmware` text_sensor. Value is set by a boot-time lambda dispatched off `radioN_protocol`:
    - `znp` → call ZNP `SYS_VERSION` probe → publish `rev` from response
    - `spinel` → probe stub → publish `"unknown (spinel probe not implemented in v1)"`
    - `ezsp` → probe stub → publish `"unknown (ezsp probe not implemented in v1)"`
    - `zwave` → probe stub → publish `"unknown (zwave probe not implemented in v1)"`
    - Any probe timeout (200 ms) or framing error → publish `"unknown (probe timeout)"` / `"unknown (framing error)"`
    - `none` → no sensor emitted (slw09u case)

- **Ship the HA snippet** in `docs/ha-integrations/smlight-firmware-update.yaml` with a short README pointing out (a) ZHA independently reports coord version, ours is orthogonal (§6e), (b) recorder-exclusion for the big JSON attribute (§4), (c) which radios are live-probed vs stubbed in v1 (§6f), (d) how the HA update-entity template treats `"unknown"` (no update entity created; user sees only the diagnostic sensors, not a broken update card).

Users who care get an update entity in HA per live-probed radio. Users who
don't get six extra diagnostic entities per radio. Zero effect on
serial-proxy traffic — the probe is a one-shot at boot before serial_proxy
attaches (§6d).

**Explicit non-scope for v1**: no flashing, no bootloader entry, no channel
switching, no destructive operation. Read-only diagnostic surface + HA-side
comparison against SMLIGHT's public catalog. Flashing lives in v2 per §13 of
the design doc.

---

## 6. Contemplation — dynamic detection of installed version

Design question: **can ESPHome read the actual version bytes off the radio
chip and expose that as a sensor?** In principle yes; in practice this is a
bigger project than the rest of this design combined, and touches the
correctness of `serial_proxy`. A summary:

### 6a. Zigbee coordinator (CC2674P10, ZNP protocol)

The Zigbee ZNP protocol has a well-defined command `SYS_VERSION` (subsystem
`0x21`, command `0x02`). Framed as:

```
[SOF=0xFE][LEN=0x00][CMD0=0x21][CMD1=0x02][FCS]
```

Response returns TransportRev, Product, MajorRel, MinorRel, MaintRel,
Revision (32-bit — the timestamp we care about, e.g. `20240716`).

Feasibility: ~100 lines of custom ESPHome C++ / lambda. Runs once at boot
before `serial_proxy` starts accepting API traffic. Round-trip <50ms.

Risks:
- Races with `zigpy-znp` on the HA side if HA reconnects during the probe. Mitigation: run before Wi-Fi/Ethernet reports up; publish result once and never touch UART1 again.
- If the user has flashed *router* firmware (`type: "1"`) instead of coord, `SYS_VERSION` may behave differently or hang. Mitigation: timeout the probe at 200ms and fall back to `radio1_installed_rev`.
- Adds ESPHome C++ code that must be maintained alongside the substitution-based flow.

### 6b. Zigbee router (CC2674P10 running ZR firmware)

Different framing / different subsystem. Would need a separate probe path.
Not widely used in our target audience — MR4U ships as coordinator.

### 6c. OpenThread RCP (EFR32MG26 running OT firmware)

Spinel protocol. Property `PROP_NCP_VERSION` (0x02) returns a version string.
Framing: HDLC-lite. Larger and more finicky than ZNP; also 460800 baud
(vs 115200 for ZNP), needs correct UART setup for the probe window. Deferred
to v1.x — v1 ships a stub for Spinel that publishes
`"unknown (spinel probe not implemented in v1)"`. See roadmap.md v1.x for the
real-probe replacement.

### 6d. v1 shipping order

Revised (2026-09-10) — the earlier "ship level 2 first, probe later" phasing
has been superseded. v1 now ships the probe mechanism uniformly across every
radio the fork's hw_defs declare, with per-protocol implementation quality
that ranges from "live" to "substitution-fallback stub." See §6f for the
concrete coverage matrix.

Dispatcher order at boot (per radio, in parallel where UARTs are independent):

1. Read `radioN_protocol` and `radioN_role` substitutions at compile time (or their HA-select override values from NVS at runtime, see §10).
2. In an `on_boot: priority: 250` lambda — after the UART bus (`setup_priority::BUS`, ~1000) has come up and well before `serial_proxy` (`setup_priority::AFTER_CONNECTION`, ~-30) attaches — invoke the appropriate probe:
   - `protocol == znp` → ZNP `SYS_VERSION` (implemented)
   - `protocol == spinel` → stub (deferred to v1.x)
   - `protocol == ezsp` → stub (deferred to v1.x)
   - `protocol == zwave` → stub (deferred to v1.x)
   - `protocol == none` → no sensor emitted
3. On success, publish parsed `rev` (YYYYMMDD string) to `radioN_installed_firmware`. On any failure — stub, timeout, framing error, unknown chip — publish `"unknown (<reason>)"`. No substitution fallback.
4. Return; UART is untouched for the remainder of runtime until `serial_proxy` claims it after HA connects.

The dispatch happens per radio, so a device with one live-probed radio and
one stubbed radio just gets a mixed result set — no all-or-nothing failure
mode. The HA update-entity template skips radios reporting `"unknown"` so a
stubbed radio just doesn't produce an update card; the diagnostic sensors
still show what the substitutions declared.

### 6e. What ZHA already reports (and why we still don't rely on it)

ZHA (via `zigpy-znp`) issues `SYS_VERSION` during coordinator startup and
exposes the result on the coordinator device in HA — typically as a
`sw_version` / `firmware_version` attribute of the form `2.7.1.20260311`.
Zigbee2MQTT surfaces the same string through its Bridge info topic.

That means for **Radio 1 only**, an HA-side template could sidestep our
substitution entirely:

```jinja
installed_version: >
  {{ state_attr('device.zigbee_coordinator', 'sw_version') | default(states('sensor.mr4u_radio_1_installed_firmware'), true) }}
```

We deliberately don't ship this as the default. Reasons:

1. **Radio 2 (EFR32MG26 Thread) has no equivalent.** ZHA doesn't touch it. HA's OT-BR integration doesn't currently surface Spinel `PROP_NCP_VERSION` as a template-visible attribute. So Radio 2 would need the ESPHome-sourced value anyway — using ZHA for one radio and ESPHome for the other bifurcates the design and doubles the template forks (ZHA vs Z2M attribute names).
2. **Uniformity is a stronger invariant than one-line convenience.** The design goal from §3 is "the ESPHome device is fully self-descriptive over Native API." Sourcing installed version from a *different* HA integration undermines that; the device stops being fully self-describing.
3. **ZHA-sourced version is silent when ZHA is stopped or hasn't started yet.** The ESPHome substitution is always defined.
4. **User doesn't lose the info.** ZHA still shows its own `firmware_version` in the coordinator device panel — that's independent of anything we ship. A user who wants live-accurate can just look there; our HA update entity is about the *comparison against SMLIGHT catalog*, which ZHA doesn't do.
5. **v1 impact is trivial.** One substitution to bump after a reflash. When the SYS_VERSION probe (level 3) lands, even that goes away.

Documenting-only decision: the shipped `docs/ha-integrations/` README should
mention that ZHA independently reports Radio 1's installed version in the
coordinator device panel, so users aren't confused about which value to
trust. Both should agree; if they don't, either the probe returned wrong
bytes (bug — file an issue) or the user is looking at a stale ZHA cache.
Our probe re-fires on every boot, so a boot cycle resolves the ambiguity.

### 6f. v1 protocol coverage matrix — SMLIGHT catalog vs our hw_defs

Cross-check of every chip family the fork's `hw_defs/**/*.yaml` can currently
declare, mapped against the SMLIGHT `type=ZB&format=slzb` catalog entries the
HA lookup will consult, and the probe protocol required.

| SMLIGHT catalog id | Chip family | Firmware type | Native protocol | hw_defs that declare it today | v1 probe |
|---|---|---|---|---|---|
| `70` | CC2674P10 | Coord (`0`) | ZNP (`SYS_VERSION`) | `mrxu/mr4u_r1_73.yaml` UART1, `ultima/r1_04.yaml` UART1 | **live** |
| `70` | CC2674P10 | Router (`1`) | ZNP (`SYS_VERSION`) | (not declared) | live (same probe) |
| `69` | CC1352P7 | Coord (`0`) | ZNP (`SYS_VERSION`) | (not declared, but SLZB-06P7 support may add it later) | live (same probe) |
| `69` | CC1352P7 | Router (`1`) | ZNP (`SYS_VERSION`) | (not declared) | live (same probe) |
| `68` | EFR32MG26 | Coord (`0`) | EZSP over ASH (`EZSP_VERSION`) | *possibly `06xu/r1_73.yaml` single-radio slot, per-variant* | **stub → v1.x** |
| `68` | EFR32MG26 | Thread (`2`) | Spinel (`PROP_NCP_VERSION`) | `mrxu/mr4u_r1_73.yaml` UART2 (verified on sampled device), `ultima/r1_04.yaml` UART2 | **stub → v1.x** |
| `67` / `91` | EFR32MG24 | Coord (`0`) | EZSP over ASH (`EZSP_VERSION`) | (not declared) | **stub → v1.x** |
| `67` / `91` | EFR32MG24 | Thread (`2`) | Spinel (`PROP_NCP_VERSION`) | (not declared) | **stub → v1.x** (same probe as EFR32MG26 Thread) |
| *n/a* (not in ZB catalog) | ZW-800 | Z-Wave 800 | Z-Wave Serial API (`FUNC_ID_ZW_GET_VERSION`) | `ultima/r1_04.yaml` UART3 | **stub → v1.x** |
| *n/a* | *(no radio)* | *(n/a)* | *(n/a)* | `slw09u/r1_01.yaml` | no probe needed |

Older SKU ids (`0`–`5`, `12`, `13`, `16`–`18`, `21`, `23`, `65`, `66`) are
either non-radio host firmwares or product lines the fork does not target;
their `hw_defs` don't exist here.

**Interpretation:**

- The **one chip live-probed in v1** is CC2674P10 in Coord mode — the fork's primary target and the only extensively tested platform per README. That covers Radio 1 on MR4U and Ultima.
- **All other protocols (Spinel, EZSP, Z-Wave) ship as stubs in v1** and are upgraded to real probes in v1.x point releases. Spinel is the most complex of the three (HDLC-lite framing + CCITT-16 CRC + unsolicited-property discard at boot) and is deferred alongside the others to keep v1 scope tight. Users of stubbed radios still see the diagnostic sensors (`chip`, `smlight_id`, `protocol`, `role`, `firmware_channel`, `uart_baud`) but `installed_firmware` reads `"unknown (<protocol> probe not implemented in v1)"`. The HA update-entity template treats these as "no update information available" and simply doesn't render an update card for that radio.
- **Adding a real Spinel probe in v1.x** upgrades MR4U Radio 2 (and Ultima UART2) from stub-`unknown` to live-probed with no user-facing entity or template change.
- **Adding EZSP later** does the same for 06xu (EFR32-coord variants) and any EFR32MG26 Coord builds.
- **Adding Z-Wave later** does the same for the Ultima's UART3.

**Bounded scope guarantee:** the dispatcher hardcodes the four protocols
above. A `radioN_protocol` value outside `{znp, spinel, ezsp, zwave, none}`
fails ESPHome build-time validation (enum-typed schema in the custom
component's Python side). No silent fall-through at runtime; the check
happens at compile.

### 6g. Chip × protocol × role permutation matrix — the user-facing complexity

This is the table users need to see (and dropdowns need to enforce) so they
don't accidentally pick nonsensical combinations. Each row is a valid
`(chip, protocol, role)` triple that can appear on real hardware. Rows
marked ❌ are invalid combinations that ESPHome build validation will reject.

| Chip | Protocol | Role | Meaning | SMLIGHT catalog id | SMLIGHT `type` | Typical baud | v1 probe |
|---|---|---|---|---|---|---|---|
| **CC2674P10** | `znp` | `coord` | TI Z-Stack coordinator | `70` | `0` | 115 200 / 460 800 | ✅ live |
| **CC2674P10** | `znp` | `router` | TI Z-Stack router (repeater firmware) | `70` | `1` | 115 200 | ✅ live (same probe) |
| CC2674P10 | `spinel`, `ezsp`, `zwave` | *any* | ❌ not shipped — TI chip can't run Silabs stacks | — | — | — | rejected |
| **CC1352P7** | `znp` | `coord` | TI Z-Stack coordinator | `69` | `0` | 115 200 / 460 800 | ✅ live (same probe as CC2674P10) |
| **CC1352P7** | `znp` | `router` | TI Z-Stack router | `69` | `1` | 115 200 | ✅ live |
| **EFR32MG26** | `ezsp` | `coord` | Silabs EmberZNet coordinator (NCP mode) | `68` | `0` | 115 200 / 460 800 | 🟡 stub → v1.x |
| **EFR32MG26** | `ezsp` | `router` | Silabs EmberZNet router | `68` | `1` | 115 200 | 🟡 stub → v1.x |
| **EFR32MG26** | `spinel` | `rcp` | OpenThread Radio Co-Processor (Thread stack runs on host / OTBR) | `68` | `2` | 460 800 | 🟡 stub → v1.x |
| EFR32MG26 | `spinel` | `ncp` | *Theoretically* OpenThread NCP mode | `68` | `2` | 460 800 | 🟡 stub → v1.x (same probe as RCP once implemented) |
| **EFR32MG26** | *(multi-PAN)* | *(concurrent)* | Multi-PAN builds run Zigbee EZSP + Thread Spinel concurrently on one chip | `68` | *(not yet in catalog)* | 460 800 | 🟡 out-of-scope for v1; would need dual-protocol dispatch |
| EFR32MG26 | `znp`, `zwave` | *any* | ❌ not shipped — EFR chip can't run TI stack or Z-Wave | — | — | — | rejected |
| **EFR32MG24** | `ezsp` | `coord` | Silabs EmberZNet coordinator, cheaper chip | `67` / `91` | `0` | 115 200 | 🟡 stub → v1.x |
| **EFR32MG24** | `ezsp` | `router` | Silabs EmberZNet router | `67` / `91` | `1` | 115 200 | 🟡 stub → v1.x |
| **EFR32MG24** | `spinel` | `rcp` | OpenThread RCP (rare on MG24 — usually Zigbee-only chip) | `67` / `91` | `2` | 460 800 | 🟡 stub → v1.x (same probe as MG26 once implemented) |
| EFR32MG24 | `znp`, `zwave` | *any* | ❌ not shipped | — | — | — | rejected |
| **ZW-800** | `zwave` | `primary_ctrl` | Z-Wave Series 800 primary controller | *not in ZB catalog* | *(separate catalog TBD)* | 115 200 | 🟡 stub → v1.x/v2 |
| ZW-800 | `znp`, `spinel`, `ezsp` | *any* | ❌ not shipped — dedicated Z-Wave silicon | — | — | — | rejected |
| ZW-800 | `zwave` | `router`, `coord`, `rcp`, `ncp` | ❌ not shipped — SMLIGHT ships only primary-controller firmware for this chip | — | — | — | rejected |
| **(none)** | `none` | *(n/a)* | Radioless board — no probe emitted (slw09u) | — | — | — | n/a |

**Reading the table (user perspective):**

- **Pick your chip first** — that's a physical property of the board, not a choice. It's declared in `hw_defs/**` by the maintainer and is not user-editable at HA runtime (changing chips means swapping silicon).
- **Then pick protocol + role together** — these are the two dimensions the *firmware image* the user flashed decides. The pair `(protocol, role)` maps 1:1 to a SMLIGHT catalog `type` field (see the "SMLIGHT `type`" column). If SMLIGHT ships an image for that pair, we can probe / compare; if not, it's `❌`.
- **Baud is inherited from the firmware** — SMLIGHT often ships the *same* protocol+role at multiple baud rates (e.g. CC2674P10 coord at both 115 200 and 460 800). The v1 probe reads `radioN_uart_baud`, and HA templates use it to disambiguate catalog entries that only differ by baud.

**Where the multi-PAN complication lives:**

EFR32 chips can run "multi-PAN" firmware where Zigbee EZSP and Thread Spinel
share the same radio via time-division multiplexing. SMLIGHT's catalog has
begun listing these builds under new `type` values. v1 explicitly does not
handle multi-PAN — a user who's flashed multi-PAN sees `\"unknown\"` from the
probe and configures the update-check against whichever protocol they mainly
use. Full support would need dual-protocol dispatch on the same UART and is
tracked as a v2 stretch goal.

**Where the invalidations get enforced:**

**Compile time only** in v1 — the ESPHome custom component's Python schema
declares `chip`, `protocol`, and `role` as enums with allowed values, plus a
cross-field validator that rejects impossible triples (`znp + rcp`,
`spinel + coord`, `ezsp + primary_ctrl`, etc.). A user mis-editing
`hw_defs/**` gets a clear `esphome config` error, not a silent runtime bug.

There is no runtime enforcement layer in v1 because there are no runtime
knobs for radio identity — see §10. The v2 flash-chain UX will add a second
enforcement layer at the HA `select` level (populated from this matrix per
chip); see [roadmap.md](roadmap.md) v2 · tier-3 UX.

The `\"unknown chip\"` runtime branch in the dispatcher remains as a
belt-and-braces log warning in case a future edit adds a chip enum value but
forgets to wire the dispatcher.

**Keeping this matrix current** is tracked in [roadmap.md](roadmap.md) under
*Ongoing / cross-cutting*. TL;DR: [docs/radio-firmware/catalog-snapshot.json](radio-firmware/catalog-snapshot.json) is a
committed snapshot; refresh via [docs/radio-firmware/refresh.py](radio-firmware/refresh.py) and
review the diff for new `(chip_id, type)` tuples. See [docs/radio-firmware/README.md](radio-firmware/) for the maintainer workflow. A CI cron job in v1.x
automates the "notice drift" step.

---

## 7. Channel selection — prod vs dev

### 7.1 Observed SMLIGHT channel policy (as of 2026-09)

Direct inspection of the SMLIGHT SLZB-OS "Radio Module OTA update" dialog on a
live MR4U reveals that SMLIGHT's "release" and "dev" channels aren't the
usual "current stable" vs "bleeding-edge nightly" split. They're closer to
"old but frozen" vs "actively maintained." Concretely, for chip `70`
(CC2674P10):

| Channel | Top rev | Build date | SDK | Notes |
|---|---|---|---|---|
| Release (`prod: true`) | `20240716` | 16 Jul 2024 | 7.41 | Marketed as "SMLIGHT latest Coordinator release" |
| Dev (`prod: false`) | `20260311` | 11 Mar 2026 | 8.32.00.07 (Dec 2025) | "Performance and Stability Optimizations"; explicitly labeled Beta |

Additional dev builds visible in the same dialog: `20260310` (460 800 baud
variant of `20260311`), `20260307` ("Industry First — 460 800 Baud on CC26xx",
first-ever 460 800 test), and older test builds down to `20250325`. Real
engineering activity clearly happens on dev.

Same story on the EFR32MG26 (chip `68`, Thread): the live device sampled for
this fork was running `20260416` dev.

Implication for us: **hardcoding `prod: true` in the HA `template: update:`
snippet would mark a dev-channel radio as "up to date" even when a newer dev
build has shipped.** For the target audience of this fork (people actively
running SMLIGHT hardware, often already on dev because release is 20+ months
stale), that's the wrong default.

### 7.2 Design choice — compile-time per-device channel substitution

Three shapes were considered:

- **A — Hardcode `prod: true`.** Simplest. Wrong for anyone on dev; also wrong for anyone whose CC is on a `20260311`-style build and would be told "up to date" by a release-only feed. Rejected.
- **B — Compile-time substitution in `hw_defs/**`.** One new key per radio, `radio1_firmware_channel: prod | dev`. Template filters accordingly. Zero runtime cost, no HA-side UI, chosen at flash time when the user builds their firmware. **Chosen.** (The value name `prod` matches the SMLIGHT catalog field verbatim, so there is no substitution-value-to-catalog-field translation layer.)
- **C — Runtime HA `select` entity per radio.** Matches SLZB-OS's dropdown behavior exactly. Rejected for v1 because it requires an ESPHome `select` component + state persistence + an HA-side filter that responds to state changes. Not worth the machinery for a decision that changes ~once per user per year.

Substitution additions (each `hw_defs/**` file, one per radio):

```yaml
# hw_defs/mrxu/mr4u_r1_73.yaml
substitutions:
  # existing radio1_* keys ...
  radio1_firmware_channel: dev          # prod | dev — default 'dev' per §7.1

  # existing radio2_* keys ...
  radio2_firmware_channel: dev          # prod | dev — default 'dev' per §7.1
```

Default is **`dev`**, matching the observed real-world state of shipped-and-updated
MR4U hardware. A user who explicitly wants stable-only can flip to `prod` in
their local override.

### 7.3 Template change

Replace the hardcoded `selectattr('prod', 'eq', true)` filter in the §4
`template: update:` snippet with one keyed off the substitution's rendered
sensor. Sketch:

```yaml
latest_version: >
  {% set chip    = states('sensor.mr4u_radio_1_smlight_id') %}
  {% set ftype   = states('sensor.mr4u_radio_1_firmware_type') %}
  {% set channel = states('sensor.mr4u_radio_1_firmware_channel') %}
  {% set entries = state_attr('sensor.smlight_firmware_catalog_raw', chip) or [] %}
  {% set filt    = entries | selectattr('type', 'eq', ftype) | list %}
  {% set filt    = filt | selectattr('prod', 'eq', channel == 'prod') | list %}
  {# newest revision first — 'rev' is a sortable YYYYMMDD string #}
  {% set filt    = filt | sort(attribute='rev', reverse=true) %}
  {{ filt[0].rev if filt else states('sensor.mr4u_radio_1_installed_firmware') }}
```

Corresponding new `text_sensor` in `packages/diagnostics/radio_firmware_info.yaml`:

```yaml
- platform: template
  name: "Radio 1 firmware channel"
  id: radio1_firmware_channel_sensor
  entity_category: diagnostic
  lambda: 'return {"${radio1_firmware_channel}"};'
```

### 7.4 Documentation stance for users

The shipped `docs/ha-integrations/smlight-firmware-update.yaml` README should:

- State the default is `dev` and *why* (release cadence is effectively frozen).
- Note that switching a live coordinator between `prod` and `dev` may involve a major SDK jump (7.41 ↔ 8.32) that can invalidate stored NIB (NVRAM) contents and force re-pairing of the Zigbee mesh. Downgrades in particular are one-way.
- Explicitly *not* recommend the release channel for CC2674P10 until SMLIGHT re-syncs it (as of 2026-09, release is ~14 months behind dev with no signs of movement).

### 7.5 Bonus — auto-detect currently-installed channel from live-probed `rev`

The `radioN_firmware_channel` substitution answers *"which channel do you
want the HA update entity to compare against?"* — not *"which channel is your
radio actually on right now?"*. The latter can be derived automatically
because the SMLIGHT catalog's `rev` field is a unique per-build identifier
that appears in exactly one of the `prod: true` / `prod: false` entries for
that chip.

For radios where v1 probes live (§6f — CC26xx ZNP only in v1), the
returned `rev` can be cross-referenced against the whole catalog to derive
the currently-installed channel:

```jinja
# HA-side derived sensor
{% set chip    = states('sensor.mr4u_radio_1_smlight_id') %}
{% set ftype   = states('sensor.mr4u_radio_1_firmware_type') %}
{% set current = states('sensor.mr4u_radio_1_installed_firmware') %}
{% set entries = state_attr('sensor.smlight_firmware_catalog_raw', chip) or [] %}
{% set match   = entries | selectattr('type', 'eq', ftype)
                        | selectattr('rev',  'eq', current) | list %}
{% if match | length == 0 %}
  custom
{% elif match[0].prod %}
  prod
{% else %}
  dev
{% endif %}
```

Output values: `prod`, `dev`, or `custom` (installed a build not in the
catalog — e.g. user compiled their own, or ran a build that's since aged out
of the catalog). This is a diagnostic view: it tells the user "you're
currently on the prod/dev channel" without them having to know.

The user-configurable `radioN_firmware_channel` substitution is still needed
separately — it controls what "latest available" resolves to (i.e. whether
HA notifies about a newer *prod* build or a newer *dev* build). Two answers
to two different questions.

For stubbed protocols (v1 EZSP/Z-Wave) the auto-detect fails cleanly to
`custom` because there's no live `rev` to match. That's fine.

---

## 8. Remaining open questions

- **Should the HA snippet be per-device or one shared block?** Current sketch is per-device (users copy per radio). Alternative: a Jinja macro in a `!include` file that takes the device slug as an arg and expands. Cleaner for multi-device homes; more machinery to explain.
- **`hwFlow: true` filter** — MR4U's UART1 has hardware flow control. Some catalog entries specify `hwFlow: true`. Do we need to match on that too, or does `type` alone suffice? Needs a quick test.
- **Should we detect chip mismatch?** If a user pins `radio1_smlight_id: "70"` but the physical chip is actually a CC1352P7 (would happen if they buy the wrong hardware revision), we'd silently offer them the wrong firmware. Mitigation: add a `radio1_chip_expected` sensor with an assertion at boot. Probably overkill.
- **User-facing "install" action** — the SMLIGHT `link` field is a direct binary download. Turning that into a one-click "install" from HA is a much bigger project (requires our ESPHome firmware to accept a download-and-reflash instruction over API, plus DTR/RTS bootloader entry, plus resume-safe writes). Out of scope here; document the manual path (download → flash via ESPHome dashboard / cc2538-bsl / commander) instead.

---

## 9. Summary

- SMLIGHT's public JSON firmware catalog is at `https://updates.smlight.tech/services/api/slzb-06x-ota.php?type=ZB&format=slzb`, keyed by chip ID, no auth, stable.
- MR4U's chips map to catalog keys `"70"` (CC2674P10, `type: "0"` coordinator) and `"68"` (EFR32MG26, `type: "2"` OpenThread).
- Encoding chip IDs and firmware type as substitutions in `hw_defs/**` lets a **single generic HA `template: update:` snippet** serve every device revision this fork supports.
- Live-probing the radio for its actual running version is possible but a much bigger project — worth building later, not needed for a first useful version.

---

## 10. Configuration surface — v1 is deliberately substitution-only

Earlier drafts of this doc argued for a tier-3 approach (YAML defaults + HA
`select` overrides + optional `web_server:` mirror). That recommendation was
**withdrawn** during design review (2026-09-10). The reasoning below explains
why v1 has *no* runtime configuration surface for radio identity, and why
that will change in v2 when we ship the flash chain.

### 10.1 The core insight — selection must equal action

Any UI dropdown that lets a user pick a `protocol` or `role` implies "picking
this makes it so." That implication is only truthful when the pick triggers
a real change on the radio (i.e. a firmware reflash). In v1 we deliberately
don't ship the flash chain — that's v2 (see design doc §13).

Without a flash chain behind the dropdown, a `select` for `protocol` would:

- Let a user pick `spinel+rcp` on a `znp+coord` radio → probe times out →
  sensor publishes `"unknown"` → user blames our probe code for something
  that was actually a misleading UI affordance.
- Present a dropdown of options that don't correspond to the physical
  reality of what firmware is loaded → violates the "no lying to the user"
  principle we hold ourselves to across the fork (see §6d, §6f).
- Encourage the mental model "changing this select changed the radio" →
  false, and the failure mode is silent (radio stays as before).

So v1's rule: **anything that describes the flashed firmware is a
compile-time substitution, edited only by the person who also decides what
to flash.** Users who reflash their radio out-of-band update their local
`hw_defs/**` and rebuild.

### 10.2 v1 config surface — pure substitutions, zero selects

| Setting | v1 shape | Where it lives | Change requires |
|---|---|---|---|
| `radioN_chip` | Substitution | `hw_defs/**` | Physically swapping silicon (never a user action) |
| `radioN_smlight_id` | Substitution | `hw_defs/**` | Same |
| `radioN_protocol` | Substitution | `hw_defs/**` | Reflashing the radio + editing YAML + rebuilding ESPHome firmware |
| `radioN_role` | Substitution | `hw_defs/**` | Same |
| `radioN_firmware_channel` | Substitution | `hw_defs/**` | Editing YAML + rebuilding (deliberately kept uniform for v1) |
| `radioN_uart_baud` | Substitution (already exists as `uartN_baud`) | `hw_defs/**` | Editing YAML + rebuilding |

Result: **v1 has no user-visible knobs at all** for radio identity. Just a
boot-time probe that reads compile-time substitutions and publishes a
`text_sensor`. Everything the user *sees* in HA is either observation
(diagnostic sensors + `installed_firmware`) or the HA-side update comparison
template that takes those sensors as inputs.

### 10.3 What v1 users still get

- Live-probed installed firmware version per radio (ZNP on CC26xx only).
- Stubbed radios (Spinel + EZSP + Z-Wave) transparently publish `"unknown (<protocol> probe not implemented in v1)"`.
- HA update entity comparing live `rev` against SMLIGHT catalog for the live-probed CC26xx ZNP protocol. Stubbed radios don't produce an update card (template filters `unknown` out).
- All other fork features (encrypted `serial_proxy` transport, OTA-password, no plaintext ports) unchanged.

### 10.4 What v1 users don't get

- No HA dropdowns for changing protocol / role / channel. Any of those is an
  editor-and-rebuild operation.
- No on-device `web_server:` dashboard for admin. Users who want a device-side
  UI can add `web_server:` themselves — it's an ESPHome-provided component
  and always available; we just don't ship it enabled by default because
  our target audience already runs HA (see README).
- No "click here to fix a mismatched probe" affordance. If the probe returns
  `unknown` because the substitution doesn't match what's flashed, the user
  edits their `hw_defs/**` copy and rebuilds. Explicit, honest, no false
  promises.

### 10.5 The v2 story — where selects and flashing land together

The tier-3 UX (HA `select` for protocol / role / channel) becomes the front-end
of a real flash operation in v2:

1. User picks `Radio 1 protocol: spinel, role: rcp` in HA.
2. HA add-on (v2-owned) queries SMLIGHT catalog for a matching Thread build.
3. Add-on displays available builds; user confirms one.
4. Add-on drives Native API to enter bootloader on the ESP32-side (DTR/RTS via `serial_proxy`), streams the image, verifies checksum, exits bootloader, resets radio.
5. Our boot-time probe re-fires with the *new* `protocol` (now stored in NVS), reads the new `rev`, publishes it. HA sees the update immediately.

In this world, **the dropdown and the reality can never diverge** because
the dropdown *is* what makes them agree. That's a coherent story. It's not
the story we can honestly ship in v1.

### 10.6 The `firmware_channel` question — why we don't split it out

Reasonable objection: `firmware_channel` doesn't describe what's flashed on
the radio (it just says "which SMLIGHT channel should HA check for updates
against?"). So arguably it could be an HA `select` in v1 without falling
into the "misleading affordance" trap.

Chosen answer: keep it as a substitution for v1 anyway, for two reasons:

1. **Uniform mental model.** "Everything about radios comes from `hw_defs/**` in v1" is a rule users can remember. Splitting one metadata knob into HA and leaving the rest in YAML introduces exactly the two-sources-of-truth ambiguity we want to avoid.
2. **Cheap to change post-v1.** Promoting `firmware_channel` to a runtime `select` in v1.x is additive — replace the compile-time substitution use with a NVS-persisted select, done. No breaking changes for users; existing installs continue to work with the substitution default.

If someone in the early user community proves this decision wrong (many
users flipping between dev and prod, tired of rebuilds), we promote it to a
`select` in a point release without any of the risk of also exposing
protocol/role selects prematurely.

### 10.7 Deferred to v2 — the whole tier-3/tier-4 discussion

The four-tier configuration UX analysis from the previous draft (YAML /
YAML+HA-selects / +web_server / custom SPA) has been moved to design doc
§13 "v2 planning" alongside the flash chain design. When we sit down to
design v2 we'll re-visit tier 3 vs tier 4 *in the context of* the flash
chain being real — which is the only context in which those tiers are
honest.

---

## 11. Deferred — configuration security surface (not blocking v1)

With v1's decision to ship substitutions only and no `web_server:` bolt-on,
the previous concerns about `web_server:` transport and `select`-mutation
authentication are moot for v1. They'll re-emerge in v2 when we design the
tier-3 flash-chain UX; that's the right time to do the auth analysis rather
than pre-emptively for a surface v1 doesn't ship.

Placeholder items for the v2 security review:

- If v2 exposes tier-3 selects over Native API, that surface is Noise-encrypted and authenticated by the API key by construction — cheap win.
- If v2 exposes an on-device `web_server:` for flash-progress display, HTTPS story on ESP32 needs a certificate-management design.
- Flash-chain writes have their own trust chain (SMLIGHT signing, image checksum, bootloader authentication) that's largely orthogonal to the "config UI" transport question.

None of this affects v1 shipping.
