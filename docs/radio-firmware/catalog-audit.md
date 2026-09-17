# Radio-firmware catalog audit

A snapshot analysis of SMLIGHT's public radio-firmware catalog
(`catalog-snapshot.json` at commit-time), produced by
`download_all.py --report-only`. This file is **descriptive**, not normative
— it captures what SMLIGHT ships today so that the code and design docs
elsewhere in this fork can be reasoned about against real data instead of
folklore.

Regenerate this document when `refresh.py` shows a non-trivial diff — see
the "Refresh workflow" section at the bottom.

## Contents

- [Refresh diff summary](#refresh-diff-summary)
- [Chip-family map](#chip-family-map-20-ids--5-chips)
- [Duplicate SHA groups](#duplicate-sha-groups)
- [Broken links](#broken-links)
- [Baud + hwFlow observed matrix](#baud--hwflow-observed-matrix)
- [The "signed MG26" fiction](#the-signed-mg26-fiction)
- [Design implications](#design-implications)
- [Refresh workflow](#refresh-workflow)

## Refresh diff summary

Comparing the pre-refresh snapshot to the current one:

- **No new catalog ids.** Still 20 top-level ids
  (`0, 1, 2, 3, 4, 5, 12, 13, 16, 17, 18, 21, 23, 65, 66, 67, 68, 69, 70, 91`).
- **No new firmware entries.** Every entry is either identical to the prior
  snapshot or has only the download URL changed.
- **Only change**: 12 URL-only rewrites on ids **21, 65, 66** where SMLIGHT is
  migrating `dl.php?file=…` → `services/api/fw-dl.php?device=…&file=…`. The
  file bytes fetched from the new URL are byte-identical to the prior URL for
  every entry that we could resolve.
- **No schema changes.** No new fields, no new `type` values, no new value
  shapes for `rev`, `baud`, `hwFlow`.

Bottom line: nothing about the catalog surface has moved. Any behaviour we
saw in the previous audit still stands.

## Chip-family map (20 ids → 5 chips)

Chip family is inferred by fingerprinting URL fragments in each entry's
`link`. Where the majority of a given id's entries all fingerprint to the
same silicon, that's the family this id serves; where the URL carries no
identifiable token, we mark it `unknown` and rely on device docs.

| id | entries | types offered | chip family | notes |
|---:|---:|---|---|---|
|  0 | 18 | 0, 1, 2 | CC1352P2 (SLZB-06 base) | URL says `slzb06/…` — no chip token in path, family verified vs SMLIGHT device page |
|  1 |  9 | 0, 1, 2 | EFR32MG21 (SLZB-06m) | URL prefix `slzb06m…`; not one of our five main families — this is an older Silabs part |
|  2 | 10 | 0, 1, 2 | EFR32MG24 | SLZB-06 "MG24" refresh — same wire as id 23, older packaging |
|  3 | 14 | 0, 1, 2 | CC1352P7 | SLZB-06P7 (internal antenna) |
|  4 | 12 | 0, 1, 2 | **CC2674P10** | **SLZB-06P10 track. This is what MR4U Radio 1 and Ultima Radio 1 ship.** |
|  5 | 18 | 0, 1, 2 | CC1352P7 | SLZB-06P7-EXT (external antenna, U variant) — byte-identical to id 17 |
| 12 |  6 | 5, 6, 7 | Silicon Labs Z-Wave (`slzb_MRW10`) | Z-Wave EU / US / ANZ — the only chip whose `type` encodes region |
| 13 |  6 | 0, 1, 2 | **EFR32MG26** | **This is what MR4U Radio 2 ships.** Also the primary dev-Thread MG26 track. |
| 16 | 11 | 0, 1, 2 | CC1352P2 (SLZB-06U) | Chip-identical to id 0 tracks, byte-duplicates for BL2 loader |
| 17 | 18 | 0, 1, 2 | CC1352P7 | Byte-identical duplicate of id 5, `EXT` and `U` marketing labels |
| 18 | 12 | 0, 1, 2 | CC2674P10 | Byte-identical duplicate of id 4 |
| 21 |  4 | 0, 1, 2 | EFR32MG26 | Unsigned MG26 SDK v8.0.3 track; first_file `slzb06Mg26U_zigbee_ncp_8.0.3.0_sw_flow_115200.gbl` |
| 23 |  7 | 0, 1, 2 | EFR32MG24 | SLZB-07 MG24 track; byte-identical to id 67 |
| 65 |  4 | 0, 1, 2 | EFR32MG24 | SMHUB signed EFR32MG24 — semver `rev`, `ncp-uart-hw-v…-slzb-07-115200.gbl` |
| 66 |  5 | 0, 2 | CC1352P2 | SMHUB signed cc26xx — mix of dates + a broken `.hex.hex` typo |
| 67 |  7 | 0, 1, 2 | EFR32MG24 | Semver `rev`; two entries are 400 due to leading-slash bug in new URL endpoint |
| 68 |  3 | 0, 1, 2 | EFR32MG26 | **"Signed" SDK v8.0.3 — bytes match id 21. See [The "signed MG26" fiction](#the-signed-mg26-fiction).** |
| 69 |  1 | 0 | CC1352P7 | SMHUB `.hex` for CC1352P7, single entry `CC1352P7_coordinator_20250321.hex` |
| 70 |  3 | 0, 1, 2 | CC2674P10 | SMHUB `.hex` — **genuinely different bytes** from id 4 (`.hex` vs `.bin`) |
| 91 |  1 | 0 | EFR32MG24 | Single-entry `siMg24_zigbee_bridge_9.1.1.0_115200.gbl` — Silabs bridge firmware |

Only two of these ids matter for MR4U as it ships today: **4** (Radio 1) and
**13** (Radio 2). Earlier commits pinned MR4U to ids 70 and 68 by URL
fragment matching against SMHUB pages — those are signed-bundle tracks for
SLZB-06 hub devices, not the raw tracks MR4U runs (see
[radio-probe-reference.md](../design/radio-probe-reference.md) §6h and the `2ce7235`
retrospective there).

## Duplicate SHA groups

35 groups of byte-identical files served under two or more catalog ids.
The `type`-collapsed pattern:

| Group | ids | payload count | Explanation |
|---|---|---:|---|
| CC1352P7 track | 5 ↔ 17 | 13 pairs | SLZB-06P7-EXT vs SLZB-06P7U — same silicon, dual marketing labels |
| CC2674P10 track | 4 ↔ 18 | 10 pairs | SLZB-06P10 in two SKUs |
| EFR32MG24 track | 23 ↔ 67 | 4 pairs | SLZB-07 dev vs signed presentation |
| EFR32MG26 SDK v8.0.3 | 21 ↔ 68 | 1 pair | See [The "signed MG26" fiction](#the-signed-mg26-fiction) |
| SLZB-06 base BL2 | 0 ↔ 16 | 1 pair | SLZB-06 vs SLZB-06U marketing |
| MG24 blank flash | 2 internal | 3 copies | Same blank `.gbl` referenced from three `type` entries within id 2 |

Practical consequence: a **single flashed device can be truthfully described
by more than one catalog id.** Any code that maps "the installed firmware
came from id N" needs to accept the whole duplicate group as an acceptable
match, not fail if only one id gets pinned.

## Broken links

Ten catalog entries do not resolve on refresh day:

| id | status | entry pattern |
|---:|---|---|
| 0, 3, 4, 5, 16, 17, 18 | HTTP 404 | `rev: 20260307` "Industry First 460800" CC26xx beta — SMLIGHT staged a rev-line across seven ids then pulled the binaries; catalog entries remain. Seven entries total. |
| 66 | HTTP 404 | Typo — filename ends `.hex.hex` for `CC1352P2_CC2652P_launchpad_coordinator_20221226.hex.hex`. Real file is `.hex`. |
| 67 | HTTP 400 × 2 | New `services/api/fw-dl.php` endpoint returns 400 for URLs whose `file=` param starts with `/`. Two entries carry `file=/20260416/…` and fail. |

None of the broken links affect firmware we consume in v1 (all our pinned
files resolve). They're recorded here so a future audit can tell "still
broken" from "newly regressed".

## Baud + hwFlow observed matrix

Produced from the raw catalog, grouped by `(chip_family, type)`:

| chip family | type | baud | hwFlow | # entries |
|---|:---:|---:|:---:|---:|
| cc1352p2 | 0 | 115200 | absent | 13 |
| cc1352p2 | 0 | 460800 | absent | 1 |
| cc1352p2 | 1 | 0 | absent | 2 |
| cc1352p2 | 1 | 115200 | absent | 2 |
| cc1352p7 | 0 | 115200 | absent | 22 |
| cc1352p7 | 0 | 460800 | absent | 8 |
| cc1352p7 | 1 | 0 | absent | 9 |
| cc1352p7 | 2 | 460800 | absent | 12 |
| cc2674p10 | 0 | 115200 | absent | 11 |
| cc2674p10 | 0 | 460800 | absent | 4 |
| cc2674p10 | 1 | 0 | absent | 4 |
| cc2674p10 | 1 | 115200 | absent | 1 |
| cc2674p10 | 2 | 460800 | absent | 7 |
| efr32mg24 | 0 | 115200 | absent | 4 |
| efr32mg24 | 0 | 115200 | true | 5 |
| efr32mg24 | 1 | 115200 | absent | 3 |
| efr32mg24 | 1 | 115200 | true | 3 |
| efr32mg24 | 2 | 460800 | absent | 4 |
| efr32mg24 | 2 | 460800 | true | 10 |
| efr32mg26 | 0 | 115200 | absent | 3 |
| efr32mg26 | 1 | 115200 | absent | 3 |
| efr32mg26 | 2 | 460800 | absent | 5 |
| efr32mg26 | 2 | 460800 | true | 2 |

Key observations:

1. **`hwFlow: true` never appears on the TI CC26xx family.** All 90+ CC26xx
   entries omit the field — TI ZNP tracks do not advertise HW flow control
   in the catalog.
2. **Spinel (`type: 2`) baud is uniformly 460800.** Zero Spinel entries at
   115200 across any chip. If a device is running Spinel, catalog-consistent
   UART baud is 460800.
3. **EZSP coord (`type: 0` on EFR32) is 115200 only.** Both MG24 and MG26
   coordinator entries are 115200; the 460800 CC26xx track has no EZSP peer.
4. **Router firmware (`type: 1`) either has `baud: 0` or `baud: 115200`.**
   Routers don't run a host UART protocol — `baud: 0` marks that; the
   `baud: 115200` entries on some CC26xx routers are historical inconsistencies.
5. **hwFlow only distinguishes tracks on EFR32.** Within EFR32MG24/MG26,
   the same `(chip, type, baud)` triple can have both hwFlow=true and
   hwFlow=absent variants — SMLIGHT ships parallel HW-flow vs SW-flow tracks.

Prior versions of the design assumed we could reject catalog entries whose
`(baud, hwFlow)` didn't match the device's UART config. This audit shows
that treating that as a hard rejection would drop legitimate entries
(Spinel-at-460800 with hwFlow absent is a real, current firmware for MG26).
The v1 design (see [radio-probe-reference.md](../design/radio-probe-reference.md) §6h)
uses this table HA-side as an *informational* diagnostic on the update
entity — never as an enforcement gate.

## The "signed MG26" fiction

Catalog id 68 is presented by SMLIGHT as the "signed" MG26 coordinator
track — the URL basename is
`slzb06Mg26U_zigbee_ncp_8.0.3.0_sw_flow_115200_signed.gbl` and the `rev`
field says `SDK v8.0.3`. Id 21 is presented as the "unsigned" track,
same SDK v8.0.3 but `rev: 20251223`.

They are byte-identical:

- SHA-256: `ed9d5714…` on both
- Size: 270,224 bytes on both

Empirically the "signed" MG26 SDK v8.0.3 track and the "unsigned" MG26
SDK v8.0.3 track are the same file, given a different label and shipped
under a different id. This matters for the design decision in
[radio-probe-reference.md](../design/radio-probe-reference.md) §6h: chip-based detection
resolves to *either* catalog id, and we shouldn't hard-fail if the
"wrong" id was pinned — the byte payload is what matters.

(For MR4U itself this is moot: MR4U runs the dev-Thread MG26 track from id
13, not the coord tracks in 21/68. But when we extend to other MG26
targets, the id 21 ↔ id 68 equivalence has to be treated as a first-class
fact, not an accident.)

The CC2674P10 signed-hex track (id 70) is **not** byte-equivalent to id 4
— id 70 ships `.hex` files, id 4 ships `.bin`. Same silicon, genuinely
different payload envelopes. So the "signed ↔ unsigned" equivalence claim
only holds for the MG26 SDK v8.0.3 case; do not generalise.

## Design implications

1. **Chip-based detection is the correct axis.** The catalog surface makes
   `smlight_id` an unreliable pin (duplicates + relabelling). Match on
   silicon detected at runtime + role advertised by installed firmware,
   present catalog fit as an informational status. This is what
   [radio-probe-reference.md](../design/radio-probe-reference.md) §6h describes.

2. **URL-only refresh churn is normal.** SMLIGHT rewrites URLs from
   `dl.php?file=…` to `services/api/fw-dl.php?device=…&file=…` in
   place, per id, over time. `refresh.py` will show diff noise even when
   nothing about the firmware changed. Only fail-loud on a schema or `rev`
   change, not on URL churn alone.

3. **Broken links exist and are OK.** The catalog has never been 100 %
   coherent — 10 entries (7 % of entries) currently don't resolve. Any code
   that walks the catalog needs to tolerate 404 / 400 gracefully rather
   than treating a bad link as an integrity failure.

4. **Duplicate ids must be treated as an equivalence class.** Design
   documents that pin a single id ("MR4U Radio 1 = id 70") are fragile —
   pin the chip and role, then treat the id as the set
   `{id : sha256_of_catalog_entry == sha256_of_installed_image}`.

## Refresh workflow

```powershell
py docs\radio-firmware\refresh.py
git diff docs/radio-firmware/catalog-snapshot.json
# review the diff:
#   URL-only churn?           -> commit snapshot; skip audit re-run
#   new ids or rev changes?   -> re-run audit + regenerate this doc
py research\radio-firmware\download_all.py                 # download + write manifest
py research\radio-firmware\download_all.py --report-only   # print audit tables
# revise catalog-audit.md with the new numbers where they moved
```

The `research/radio-firmware/binaries/` directory and `manifest.json` are
kept entirely local (research/ is gitignored) — the firmware payloads are
vendor property and the manifest is a maintainer working file.
