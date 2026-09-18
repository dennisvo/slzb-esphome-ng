# Home Assistant integrations

Drop-in HA config snippets that pair with this fork's ESPHome build. Each
file in this directory is designed to be copied into your `configuration.yaml`
(or a HA `packages/*.yaml` file) with minimal editing.

## Contents

- **[`smlight-firmware-update.yaml`](smlight-firmware-update.yaml)** — per-radio
  "update available" entity for the ESPHome host itself (compares live-probed
  radio firmware vs. SMLIGHT's public catalog).
- **[`zha-ota-provider.md`](zha-ota-provider.md)** — enabling firmware updates
  for the Zigbee end-devices paired *through* the radio (Hue switches, Aqara
  sensors, Tuya plugs, …). One-line YAML to add the community-mirror OTA
  provider, plus the exact-match `type:` gotcha that eats an afternoon
  otherwise.

## `smlight-firmware-update.yaml` — per-radio "update available" entity

Shows, per radio, whether SMLIGHT has a newer firmware than what's installed.
Compares the ESPHome device's live-probed installed version against
SMLIGHT's public firmware catalog and renders a native HA update card.

### Prerequisites

- An ESPHome device flashed with this fork (v1 or later). The device must
  publish the per-radio diagnostic sensors — that's automatic when the
  device YAML includes `packages/diagnostics/radio_probe_ext.yaml` plus
  one or more of `radio{1,2,3}_probe.yaml`.
- HA has adopted the device via the ESPHome integration.
- HA can reach `updates.smlight.tech` outbound (HTTPS).

### Install steps

1. Copy the whole file into HA. Two ways:
   - **`configuration.yaml` append**: paste the contents (skip the top
     comment block) at the bottom of `configuration.yaml`.
   - **HA `packages/` entry** (recommended): save it as
     `packages/smlight_firmware_update.yaml` and reference the packages
     directory once from `configuration.yaml`:
     ```yaml
     homeassistant:
       packages: !include_dir_named packages
     ```
2. Personalise the `- update:` block. For each `(device, radio)` pair
   you want an update card for, duplicate the block and replace the two
   placeholder tokens:
   - `!DEVICE_SLUG!` — the ESPHome device slug in HA. Look under
     **Settings → Devices & Services → ESPHome → (your device)** and use
     the entity slug prefix. For MR4U it's typically `mr4u_r1_73`; for
     Ultima `ultima_r1_04`; for the 06xU `06xu_r1_73`.
   - `!RADIO!` — one of `radio_1`, `radio_2`, `radio_3` depending on
     which radio slot you're targeting.
3. **Add the recorder exclusion** (commented at the bottom of the
   snippet). The raw catalog sensor stores a ~15 KB JSON payload — fine
   functionally but pollutes the recorder DB.
4. Restart HA (or reload the affected YAML domain).

### Per-radio coverage in v1

The update card only appears for radios whose ESPHome-side firmware
version is live-probed. Radios published by a stub read
`"unknown (<protocol> probe not implemented in v1)"` and the template's
`availability:` filter suppresses the card until the real probe ships in
v1.x.

| Board | Radio 1 | Radio 2 | Radio 3 |
|---|---|---|---|
| MR4U (CC2674P10 + EFR32MG26) | ✅ live (ZNP) | ✅ live if Thread (Spinel); 🟡 stub if Zigbee (EZSP, → v1.x) | — |
| Ultima (CC2674P10 + EFR32MG26 + ZW-800) | ✅ live (ZNP) | ✅ live if Thread (Spinel); 🟡 stub if Zigbee (EZSP, → v1.x) | 🟡 stub → v1.x (Z-Wave) |
| 06xU (single radio, per variant) | ✅ live for CC2674P10 / CC1352P7 (ZNP) and EFR32 Thread (Spinel); 🟡 stub for EFR32 Zigbee (EZSP) | — | — |
| SLW09U (no radio) | *not included* | — | — |

Stubbed radios still expose the static diagnostic sensors (`chip`,
`protocol`, `role`, `firmware_channel`, `uart_baud`, `installed_firmware`)
so the HA snippet keeps working across the v1 → v1.x transition without
edits — the card just starts rendering the moment a live probe ships.

### Why there are no wire-derived chip / role sensors

An earlier v1 draft published `sensor.<slug>_<radio>_chip_probed` and
`sensor.<slug>_<radio>_role_probed` alongside `installed_firmware`.
Flashed testing on an MR4U revealed the wire evidence we could extract
wasn't accurate enough to earn its complexity — the ZNP `SYS_VERSION`
Product byte is a Z-Stack build id (not a chip family), and
`UTIL_GET_DEVICE_INFO` is optional in SMLIGHT's CC2674P10 builds. The
sensors were removed. See
[radio-probe-reference.md §6i](../design/radio-probe-reference.md#6i-wire-derived-chip--role--considered-and-rejected)
for the full analysis.

The practical replacement: the declared `radio*_chip` /
`radio*_protocol` / `radio*_role` sensors publish the values baked into
`hw_defs/**` (authoritative per the operator's reflash contract in
[radio-firmware-mgmt.md §6](../design/radio-firmware-mgmt.md#user-responsibility-documentation-contract)),
and the wire-derived `installed_firmware` rev either lands in the
SMLIGHT catalog track HA iterates for the update entity — or it
doesn't, in which case the update card silently declines to render and
the user sees the raw wire response on `installed_firmware_raw`.

### How catalog matching works

The template joins two data sources per (device, radio):

- **The ESPHome device's declared values** (`chip`, `role`,
  `firmware_channel`, `uart_baud`) — set in your `hw_defs/**/*.yaml`.
- **SMLIGHT's public catalog** — one `rest:` sensor fetches the whole
  catalog once per day and caches it as JSON attributes.

The catalog groups firmwares under numeric ids, but some SLZB SKUs ship
byte-identical firmware under two different ids (marketing labels
`SLZB-06P7` vs `SLZB-06P7-EXT`, "signed" vs "unsigned" MG26 SDK v8.0.3,
etc.). The template accounts for this by mapping each chip family to
its full **duplicate-SHA equivalence group**:

| Declared `chip` | Catalog ids scanned |
|---|---|
| `cc2674p10` | 4, 18 |
| `cc1352p7` | 5, 17 |
| `cc1352p2` | 0, 16 |
| `efr32mg26` | 13, 21, 68 |
| `efr32mg24` | 23, 67 |

Within a scan set, entries are filtered by `(type, baud, prod)` matching
the device's declared `role` / `uart_baud` / `firmware_channel`, and the
lexicographically-highest `rev` is offered as the update target.

**Custom channel** (`firmware_channel: custom`) short-circuits the
comparison — `latest_version == installed_version` and the release
summary calls out that the catalog is bypassed. Ids outside these
duplicate-SHA groups (SMHUB `.hex` variants like `70`, older refreshes
like `2`/`3`, signed SMHUB tracks like `65`/`66`) are deliberately not
iterated in v1; users on those tracks see the card offer no updates.
Full ids-vs-chip audit lives in
[../radio-firmware/catalog-audit.md](../radio-firmware/catalog-audit.md).

> **⚠️ If SMLIGHT restructures the catalog, this template needs
> updating.** The Jinja depends on a handful of specific field shapes:
> `chip_to_ids` mapping stays valid only while SMLIGHT keeps serving
> the same chip family under the same numeric ids; `type` values remain
> `'0'` / `'1'` / `'2'` for coord/router/rcp; `prod` remains a boolean;
> `baud` remains an integer; `rev` remains lexicographically-sortable
> within a chip family. If any of that changes, symptoms will be
> "no update card renders", "card offers a suspicious rev", or a
> template error in HA's log. Please open an issue against this repo
> so we can update the mapping.

### Multiple SMLIGHT devices

You only need **one** `rest:` block regardless of how many SMLIGHT
devices you have — the catalog is device-independent. Add one `- update:`
block per (device, radio) pair.

### ZHA already shows Radio 1 firmware — why publish it again?

ZHA reads `SYS_VERSION` from CC26xx coordinators internally and shows
the value on the coordinator device page as `sw_version`. Both should
agree with the ESPHome-side value; if they don't, either the probe
returned wrong bytes (please file an issue) or ZHA's cache is stale
(reboots resolve it).

The reason this fork ships its own probe anyway:

- **Radio 2 / Radio 3 have no equivalent** — HA's OTBR and Z-Wave JS
  integrations don't currently surface installed firmware as a
  template-readable state, so we need our own source of truth for them.
- **Uniform behaviour across radios** — the HA update-entity template
  works the same for all radios, live or stubbed, without one code path
  going through ZHA and another through ESPHome.
- **The comparison is what's new** — ZHA doesn't check SMLIGHT's
  catalog. This snippet does.

### Manual re-flash out of band

If you re-flash a radio outside this fork (e.g. SLZB-OS's web flasher),
the ESPHome-side probe will pick up the new version the next time the
device boots — the value is live, not user-declared, so no YAML edit is
required.

### Reference

- Detailed design: [../radio-probe-reference.md](../design/radio-probe-reference.md)
- Roadmap for the stub → live probe transition:
  [../roadmap.md](../design/roadmap.md) v1.x section
- SMLIGHT catalog snapshot for reviewing schema drift:
  [../radio-firmware/](../radio-firmware/)
