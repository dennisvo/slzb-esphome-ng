# ZHA end-device OTA — enabling firmware updates for Zigbee devices

Vendor-agnostic Zigbee end-devices (Hue switches, Aqara sensors, Tuya
plugs, Ikea remotes, …) can be firmware-updated over-the-air through
ZHA, but the current HA UI does **not** expose OTA provider configuration
anywhere. Providers are opted in via YAML in `configuration.yaml`, or
they don't run at all — with the sole exception of the five bundled
first-party providers, which are on by default.

This doc is the one-liner you need, plus the gotcha that eats an
afternoon if you don't know it.

## Bundled providers (already active — no YAML needed)

Latest zigpy activates these automatically. You do **not** need to list
them:

- IKEA (TRÅDFRI / DIRIGERA)
- Inovelli
- LEDVANCE / OSRAM
- SONOFF / ITEAD
- ThirdReality (3reality)

If your device is from one of those vendors, an `update.<device>_firmware`
entity should already appear once the device has reported its
`CurrentFileVersion` (may require a wake / reconfigure — see below).

## Extra provider: Koenkk community mirror (recommended)

`Koenkk/zigbee-OTA` is a community-maintained superset covering Philips
Hue, Aqara, Tuya, Develco, Dresden Elektronik, and many others. Zigpy
has a built-in provider that reads its `index.json` directly online.

Append to `configuration.yaml`:

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: z2m
```

Then Developer Tools → YAML → Check Configuration → Restart HA.

That's the whole setup. No add-ons, no cron jobs, no disk usage — zigpy
fetches `index.json` on demand and streams individual `.zigbee` binaries
directly when a device requests an update check.

## Verifying it worked

After the restart:

1. Open the Zigbee device's page in HA (e.g. your Hue switch).
2. Three-dot menu → **Reconfigure**.
3. Wake the device (press any button on a battery-powered device).
4. Refresh the page. **Firmware** should show a hex version like `0x24014120`.
5. If Koenkk's index has a higher `fileVersion` for that manufacturer +
   image type, an `update.<device>_firmware` entity appears within a
   few minutes.

If step 4 still shows `unknown`, the device hasn't reported yet — wake
it again. Battery devices often only phone home every few hours.

## The gotcha that costs an afternoon

**Provider names in `type:` must match zigpy's registered `NAME`
strings exactly.** Naming is inconsistent with what you'd guess from
reading blog posts or older docs. From
[`zigpy/ota/providers.py`](https://github.com/zigpy/zigpy/blob/dev/zigpy/ota/providers.py):

| `type:` value | What it does |
|---|---|
| `z2m` | Remote Koenkk mirror (recommended default) |
| `z2m_local` | Local Koenkk-format `index.json` on disk |
| `zigpy_remote` | Custom remote in zigpy-native index format |
| `advanced` | Scan a local folder, auto-parse every `.zigbee` / `.ota` binary |

Common mistakes that cause `Unknown OTA provider: 'xxx'` on ZHA startup:

- `advanced_file` (wrong — it's `advanced`)
- `local` (wrong — it's `advanced` for a folder, `z2m_local` for an
  index file)
- `zigbee2mqtt`, `koenkk`, `zigbee_ota` (all wrong — it's `z2m`)

If ZHA refuses to load with a `MultipleInvalid: Unknown OTA provider`
error, the `type:` string is the first thing to check.

## Alternative: fully offline (`advanced` local folder)

Only worth doing if you want offline resilience or to pin a specific
firmware image. Requires an SSH-capable add-on
(e.g. Frenck's Advanced SSH & Web Terminal) plus a periodic mirror of
`Koenkk/zigbee-OTA` — cron via that add-on's `init_commands`, or a
manual pull.

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: advanced
          path: /config/zigpy_ota/
```

The provider recursively scans that path, parses every OTA image header,
and serves any binary whose header matches a device's manufacturer +
image type. Folder structure inside `path:` is irrelevant — put files
wherever.

Trade-offs vs. `z2m`: several hundred MB on disk, staleness risk,
maintenance burden, one extra moving part (SSH add-on + mirror script).
Coverage is identical to `z2m` if you mirror the whole repo.

## Why HA has no UI for this

Historically ZHA had a Configure Provider dialog. It was removed when
zigpy started auto-enabling the first-party providers, because 90 % of
users needed no config at all. The trade-off is that the remaining
10 % (community mirror, local folder) now has to go through YAML —
which is fine once you know the exact `type:` string, and painful
until you do. Hence this doc.

## See also

- Upstream provider source (authoritative list of `NAME` values):
  <https://github.com/zigpy/zigpy/blob/dev/zigpy/ota/providers.py>
- Koenkk community OTA repo:
  <https://github.com/Koenkk/zigbee-OTA>
- ZHA docs (integration setup, not OTA):
  <https://www.home-assistant.io/integrations/zha/>
