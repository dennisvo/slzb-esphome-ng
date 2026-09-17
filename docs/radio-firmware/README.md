# Radio firmware — committed reference

This folder holds a **committed snapshot** of SMLIGHT's radio-firmware catalog
plus the script that produces it. It is a **maintainer-facing reference**, not
a runtime input.

## What lives here

| File | Purpose |
|---|---|
| `catalog-snapshot.json` | Canonicalized snapshot of the SMLIGHT `?type=ZB` catalog. Sorted keys, 2-space indent, trailing `\n`. Regenerate via `refresh.py`. |
| `refresh.py` | ~30-line fetcher. Downloads the live catalog, validates it's real JSON, canonicalizes, writes `catalog-snapshot.json`. Run manually before opening a PR that touches radio-firmware assumptions. |
| `catalog-audit.md` | Findings from the most recent audit run. Descriptive (what SMLIGHT ships) not normative (what we accept). Rewrite when a refresh surfaces new ids, new rev formats, or new chip families. |
| `README.md` | This file. |

The binary-download audit tool (`download_all.py`) and its output
(`binaries/<id>/<basename>` + `manifest.json`) live under `research/radio-firmware/`
and are gitignored — they hold vendor firmware payloads and are maintainer-
internal. See [catalog-audit.md](catalog-audit.md) for the audit findings
themselves, which are published.

## What this is *not*

- **Not what Home Assistant polls.** The HA `template update:` entity in
  [../ha-integrations/smlight-firmware-update.yaml](../ha-integrations/smlight-firmware-update.yaml)
  (when it ships) polls the *live* SMLIGHT URL, not this file. This snapshot is
  never read at ESPHome build time or at HA runtime.
- **Not a copy of firmware binaries.** The `.hex` / `.gbl` payloads themselves
  stay on SMLIGHT's CDN. This catalog only carries *pointers* to them.

## Why we commit it

1. **Matrix-drift detection.** `git diff catalog-snapshot.json` after every
   refresh shows any new `(chip_id, type, prod)` tuple SMLIGHT ships. That's
   the trigger to update the compile-time validator in
   `components/radio_probe/__init__.py` and the §6g permutation matrix in
   [../radio-probe-reference.md](../design/radio-probe-reference.md).
2. **Reproducible builds.** A user cloning the fork years from now sees the
   catalog shape we designed against. If SMLIGHT ever changes the schema,
   this is the reference for "what we designed against."
3. **Lightweight supply-chain signal.** If SMLIGHT's endpoint ever gets
   compromised or serves malformed data, the manual-refresh + git-diff loop
   is the moment someone looks.
4. **Test fixture.** The HA-side Jinja filter chain
   ([../radio-probe-reference.md](../design/radio-probe-reference.md) §7.3) can be unit-tested
   against this file: *given this catalog + these substitutions, does the
   expected `rev` come out?*

## Refresh workflow

```powershell
py docs\radio-firmware\refresh.py
git diff docs/radio-firmware/catalog-snapshot.json
# review the diff — new tuples? schema changes? unexpected removals?
# for URL-only churn, commit the snapshot and stop.
# for anything else, re-run the audit:
py research\radio-firmware\download_all.py                 # downloads + writes manifest
py research\radio-firmware\download_all.py --report-only   # prints audit tables
# revise catalog-audit.md if numbers moved; commit.
```

Cadence: manual. Refresh when preparing a release, when a user reports a
"your matrix rejects my firmware" issue, or when the v1.x
`smlight-catalog-diff.yml` CI cron flags a change (see
[../roadmap.md](../design/roadmap.md)).

## Source

Upstream URL (as at the last refresh):

```
https://updates.smlight.tech/services/api/slzb-06x-ota.php?type=ZB&format=slzb
```

Public JSON, no auth. We only fetch the `type=ZB` feed — the `type=ESPs3`
feed carries SLZB-OS host-firmware pointers we don't consume (our fork
ships its own ESPHome image).

## Provenance

This snapshot is a *verbatim canonicalized copy* of SMLIGHT's published
catalog. Attribution: © SMLIGHT. Content unchanged from upstream apart from
JSON canonicalization (key sort + reindentation).

---

## Schema (as observed)

The snapshot's structure is *implicit* — SMLIGHT publishes no schema. What
follows is what a full traversal of the file reveals; treat it as descriptive
rather than normative. If a refresh surfaces new fields or values, the drift
signal is exactly what this folder exists to catch.

### Top level

```jsonc
{
  "0":  [ /* array of firmware entries */ ],
  "1":  [ /* ... */ ],
  // ...
  "91": [ /* ... */ ]
}
```

- Keys are **chip IDs** — SMLIGHT-internal numeric strings (not IEEE part numbers, not JSON integers)
- Values are **arrays of firmware entries**, typically 1–18 per chip
- The snapshot observed at time of writing lists 20 chip IDs: `0, 1, 2, 3, 4, 5, 12, 13, 16, 17, 18, 21, 23, 65, 66, 67, 68, 69, 70, 91`

Mapping chip ID to silicon isn't published; we infer it from the `link`
filenames plus SMLIGHT's device docs. Chips we care about for v1 (see
[radio-probe-reference.md](../design/radio-probe-reference.md) §6g for the permutation
matrix that maps hw_defs into these IDs, and [catalog-audit.md](catalog-audit.md)
for the full per-id inventory including duplicate-sha groups and broken links):

| chip_id | Silicon / track | First seen in our devices |
|---:|---|---|
| 4 | TI CC2674P10 (`slzb06p10/*/znp-*.bin`) | MR4U Radio 1, Ultima Radio 1 (verified vs live probe rev 20260311; byte-identical to id 18) |
| 13 | Silicon Labs EFR32MG26 dev-Thread (`slzb06Mg26/*/slzb06Mg26_openthread_rcp_*.gbl`) | MR4U Radio 2 (verified vs live probe 3.0.1/20260416, `hwFlow: true`) |
| 67 | Silicon Labs EFR32MG24 (SLZB-07 packaging) | *v1.x extension* — byte-identical to id 23 |
| 69 | TI CC1352P7 | *v1.x extension* |
| 91 | Silicon Labs EFR32MG24 (ZREL packaging) | *v1.x extension* |

Chip IDs `68` (SMHUB signed EFR32MG26 track) and `70` (SMHUB signed CC2674P10
track) were the initial mapping we committed for MR4U in 2ce7235 — they were
inferred by URL-fragment matching against SMHUB device pages and turned out
to point at signed .gbl bundles for SLZB-06 hub devices rather than the raw
tracks MR4U actually runs. Confirmed empirically by cross-referencing the
live NCP_VERSION string (`SL-OPENTHREAD/3.0.1.0…; EFR32; Apr 16 2026`)
with catalog entry `13`. Keep these two IDs on the radar for any future v2
device that ships stock SLZB-OS.

id 68's "signed" MG26 `.gbl` is **byte-identical** to the "unsigned" id 21
track (same SHA-256 `ed9d5714…`). See
[catalog-audit.md#the-signed-mg26-fiction](catalog-audit.md#the-signed-mg26-fiction).
id 70's signed `.hex` is *not* byte-equivalent to id 4's unsigned `.bin` —
same silicon, different payload envelopes.

The IDs below 65 are older SLZB-06 family variants — same chip families in
different device housings / antenna configurations. Most of them are still
in active use by the catalog (id 4 is the primary CC2674P10 dev track, id
13 is the primary MG26 dev-Thread track), so "below 65" no longer implies
"legacy".

### Firmware entry

Every entry object carries a subset of these seven fields (nothing else was
observed in the current snapshot):

| Field | JSON type | Optional? | Meaning |
|---|---|---|---|
| `type` | string (numeric) | required | Firmware role code — see enum below |
| `baud` | integer | required | UART baud rate the firmware expects, or `0` for router (no host UART) |
| `rev`  | string | required | Version tag — format varies by chip family (see quirks) |
| `link` | string (URL) | required | Direct download of the flashable image (`.bin`, `.hex`, `.gbl`) |
| `notes`| string (HTML)   | required | Human-readable release notes, includes `<b>`, `<a>`, `<ul>` markup |
| `prod` | boolean | *optional* | `true` = official production release, `false` = beta/dev, **absent** on many older entries |
| `hwFlow` | boolean | *optional* | `true` = firmware expects HW RTS/CTS on the host UART, absent = firmware does not use HW flow control |

### The `type` enum

| Value | Meaning | Seen on chips |
|:---:|---|---|
| `"0"` | Zigbee **Coordinator** (ZNP host protocol on CC26xx, EmberZNet on EFR32) | 0, 3, 4, 5, 16, 17, 18, 65, 66, 67, 68, 69, 70, 91 |
| `"1"` | Zigbee **Router** — participates in mesh; no host UART protocol → `baud: 0` on ~57% of entries, `baud: 115200` on the rest (see catalog-audit.md for the split; both variants are current) | 0, 1, 3, 4, 5, 17, 18, 65, 66, 67, 68 |
| `"2"` | Thread / OpenThread **RCP** (Spinel over UART) | 1, 2, 13, 21, 23, 65, 67, 68, 70 |
| `"5"` | Z-Wave **EU** region | 12 |
| `"6"` | Z-Wave **US** region | 12 |
| `"7"` | Z-Wave **ANZ** region | 12 |

Not observed but plausibly reserved: `"3"`, `"4"` (possibly Matter / other
protocols, or historical). If they appear on refresh, the `smlight-catalog-diff.yml`
CI cron flags them.

### Field quirks — worth knowing before writing filters

- **`baud: 0` for many routers, `baud: 115200` for others.** Router firmwares don't speak a host UART protocol — they participate in the Zigbee mesh directly. 17 of 30 `type: "1"` entries in the current snapshot carry `baud: 0`; the remaining 13 (older CC26xx ids) carry `baud: 115200` — historical inconsistency (see catalog-audit.md §"Baud + hwFlow observed matrix"). A naive filter of `selectattr('baud', 'eq', uart_baud)` will drop the `baud: 0` half. See [roadmap.md](../design/roadmap.md) v1.x for the filter-conditional follow-up.
- **`rev` format is not uniform.** Formats observed in the current snapshot:
  - `YYYYMMDD` (most CC26xx entries): `"20260311"`
  - Semver-like: `"3.0.1.0"`, `"8.0.2.0"` (EFR32MG24 slzb-07 packaging)
  - Human SDK strings: `"SDK v8.0.3"`, `"2.7.2 sdk 2025.6.2"` (EFR32MG26)
  - The live boot probe returns whatever the *radio firmware* self-reports (e.g. a build tag), which may not string-equal the catalog `rev`. See [roadmap.md](../design/roadmap.md) v1.x "rev is not uniformly YYYYMMDD".
- **`prod` is sometimes absent.** Many older entries omit the field. Our Jinja filter `selectattr('prod', 'eq', channel == 'prod')` drops absent-`prod` entries — see [roadmap.md](../design/roadmap.md) v1.x for the semantics decision.
- **Z-Wave regional split.** For Z-Wave chips (currently only chip 12), the `type` value encodes region rather than role. A user's `radio*_role: primary_ctrl` alone doesn't identify a firmware — we also need a region. See [roadmap.md](../design/roadmap.md) v1.x under "Replace Z-Wave stub".
- **`hwFlow: true` is real.** Six chip IDs currently ship at least one firmware requiring HW flow control (chips 2, 13, 21, 23, 65, 67). MR4U's chips 68 and 70 do not — but future MG24 support will.
- **Notes carry HTML.** Any UI that renders `notes` needs to sanitize/allow inline HTML — SMLIGHT ships `<b>`, `<i>`, `<a>`, `<ul>`, `<li>`, `<br>`, `<small>` tags routinely. The HA `update:` template can render a subset via `release_summary:`.

### How this maps to our five-axis filter

Every catalog entry is uniquely identified for our purposes by:

```
(chip_id, type, baud, hwFlow, prod)
```

Which we source from hw_defs substitutions + an HA-side chip → canonical
catalog id map (see [../ha-integrations/smlight-firmware-update.yaml](../ha-integrations/smlight-firmware-update.yaml)):

```
(chip_to_id[radio*_chip], role → type, uart*_baud, uart*_hw_flow, firmware_channel)
```

The chip_to_id map is `{cc2674p10: '4', cc1352p7: '5', cc1352p2: '0',
efr32mg26: '13', efr32mg24: '23'}` — one canonical id per chip family.
Duplicate ids (e.g. 4↔18, 13↔66, 21↔68) are iterated as part of each chip's
duplicate-SHA equivalence group in the HA update template (see
[../ha-integrations/README.md](../ha-integrations/README.md) "How catalog
matching works").

The one-way mappings and edge cases:

- `radio*_role: coord`  → `type: "0"`
- `radio*_role: router` → `type: "1"` and **baud match is skipped** (all router entries have `baud: 0`) — currently unhandled in the Jinja, see [roadmap.md](../design/roadmap.md) v1.x
- `radio*_role: rcp`, `ncp` → `type: "2"`
- `radio*_role: primary_ctrl` on a Z-Wave chip → one of `"5"`, `"6"`, `"7"` depending on `radio*_zwave_region` — this substitution axis does not yet exist (v1.x)
- `firmware_channel: prod` → `prod: true`; `firmware_channel: dev` → `prod: false`; absent-`prod` entries are dropped
- `uart*_hw_flow: true` → `hwFlow: true`; `uart*_hw_flow: false` → `hwFlow` absent or `false`

For which `(chip, protocol, role)` triples we accept in v1, see
[radio-probe-reference.md](../design/radio-probe-reference.md) §6g. That's the
**normative** subset; this README describes the **descriptive** superset
SMLIGHT ships.

## Before you re-flash the Zigbee / Thread radio

This fork doesn't ship a radio-flashing flow yet — that's v2 (see
[`roadmap.md`](../design/roadmap.md)). If you re-flash the CC26xx (Zigbee) or EFR32
(Thread) radio using SMLIGHT's web flasher, a temporary SLZB-OS boot, or any
other external tool, **back up your Zigbee network state from Home Assistant
first**.

Some flashing paths wipe the radio's non-volatile memory (network key, PAN ID,
address table, frame counters). Without a backup you'd have to re-pair every
device.

- **Take the backup**: Settings → Devices & Services → ZHA → coordinator
  device → three-dot menu → Download diagnostics. Store the JSON off the
  SLZB.
- **If flashing wipes NV**: restore via ZHA → Configure → Restore automatic
  backup (or upload the JSON). The coordinator comes back on the same
  network key; paired devices reconnect without re-pairing. Battery-powered
  sleepy devices may need a wake-up press.
- **What's not at risk**: the IEEE (MAC) address lives in the radio's ROM.
  Flashing can't touch it.

For a tool-independent backup that also works with Zigbee2MQTT, `zigpy backup`
produces a portable JSON — see the `zigpy-cli` docs.
