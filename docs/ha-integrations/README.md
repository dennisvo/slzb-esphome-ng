# Home Assistant integrations

Drop-in HA config snippets that pair with this fork's ESPHome build. Each
file in this directory is designed to be copied into your `configuration.yaml`
(or a HA `packages/*.yaml` file) with minimal editing.

## `smlight-firmware-update.yaml` — per-radio "update available" entity

Shows, per radio, whether SMLIGHT has a newer firmware than what's installed.
Compares the ESPHome device's live-probed installed version against
SMLIGHT's public firmware catalog and renders a native HA update card.

### Prerequisites

- An ESPHome device flashed with this fork (v1 or later). The device must
  publish the per-radio diagnostic sensors — that's automatic when the
  device YAML includes `packages/diagnostics/radio_probe_ext.yaml` plus
  one or more of `radioN_firmware_info.yaml`.
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
| MR4U (CC2674P10 + EFR32MG26) | ✅ live (ZNP) | 🟡 stub → v1.x (Spinel) | — |
| Ultima (CC2674P10 + EFR32MG26 + ZW-800) | ✅ live (ZNP) | 🟡 stub → v1.x (Spinel) | 🟡 stub → v1.x (Z-Wave) |
| 06xU (single radio, per variant) | ✅ live for CC2674P10 / CC1352P7 (default); 🟡 stub for EFR32MG26 / EFR32MG24 | — | — |
| SLW09U (no radio) | *not included* | — | — |

Stubbed radios still expose the static diagnostic sensors (`chip`,
`smlight_id`, `protocol`, `role`, `firmware_channel`, `uart_baud`) so
the HA snippet keeps working across the v1 → v1.x transition without
edits — the card just starts rendering the moment a live probe ships.

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

- Design authority: [../v1-radio-firmware.md](../v1-radio-firmware.md)
- Roadmap for the stub → live probe transition:
  [../roadmap.md](../roadmap.md) v1.x section
- SMLIGHT catalog snapshot for reviewing schema drift:
  [../radio-firmware/](../radio-firmware/)
