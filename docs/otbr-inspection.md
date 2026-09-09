# HA OTBR add-on inspection

**Phase 0.5 inspection artifact.**
Ground-truth reference for the OpenThread Border Router add-on, its current `socat`-PTY architecture, and what the design doc's Phase 4 "internal PTY adapter" needs to build to replace it.

| Field | Value |
|---|---|
| Upstream inspected | [`home-assistant/addons`](https://github.com/home-assistant/addons) `master`, add-on `openthread_border_router` (v3.1.2), plus a comparison scan of `silabs-multiprotocol` |
| Date | 2026-09-09 |
| Method | Raw-file fetches of `Dockerfile`, `config.yaml`, and the s6-overlay `run` scripts + focused text search |
| Scope | (1) `otbr-agent` CLI surface and URL grammar; (2) current PTY/socat plumbing; (3) which Python libs are already in the runtime image; (4) what the Phase 4 replacement service must do |

---

## 1. Executive summary

The OTBR add-on is a Debian-based container that compiles OpenThread (`ot-br-posix`) from source and runs `otbr-agent` under s6-overlay supervision. **`otbr-agent` accepts only POSIX serial device paths** in its `spinel+hdlc+uart://…` URL — there is no first-class URL scheme for a network-tunneled RCP, and none of the CMake options exposed by the add-on's Dockerfile suggest OpenThread will grow one. To support a networked RCP today, the add-on uses `socat` to bridge a **plaintext** TCP endpoint to a PTY at `/tmp/ttyOTBR` and points `otbr-agent` at that PTY.

Three concrete findings that shape the design:

- **`serialx` is already installed** in the OTBR runtime image (`pip install ... serialx==${SERIALX_VERSION}` in the Dockerfile). So is `universal-silabs-flasher`. `socat` is present. `python3` + `python3-pip` are present. **All the tooling our Phase 4 PTY adapter needs is already in the container** — we don't have to redesign the base image.
- **`otbr-agent` requires a POSIX serial device path.** The compile flag `-DOT_POSIX_RCP_HDLC_BUS=ON` means only `spinel+hdlc+uart://<posix-path>` is compiled in. Rewriting `otbr-agent` to accept a `serialx` URL would mean a fork of OpenThread C++ source. Way out of scope.
- **The current `socat` architecture is exactly the plaintext-TCP problem this project exists to eliminate.** The add-on ships an s6 service `socat-otbr-tcp` that shells out to `/usr/bin/socat -d pty,… tcp:${network_device}`. `network_device` is a free-form string set by the user to `host:port` — the ESP-based gateway's `stream_server` TCP port in SLZB-OS today. Replacing this with an encrypted-API-native PTY adapter is a small, self-contained change.

**Consequence for our design:** the design doc §11 architecture ("internal PTY adapter" inside the OTBR add-on) is confirmed as the right shape. Phase 4 becomes a well-defined engineering task: add one s6 service (or fork the `socat-otbr-tcp` service) that opens `esphome-hass://` via `serialx`, creates a PTY via `os.openpty()`, symlinks it to `/tmp/ttyOTBR`, and bidirectionally forwards bytes until `otbr-agent` disconnects. `serialx` being pre-installed means the adapter is a small Python script, not a Dockerfile change.

---

## 2. Add-on structure

Two OTBR-related add-ons in `home-assistant/addons`:

| Add-on | Purpose | Relevant to us? |
|---|---|---|
| **`openthread_border_router`** | Standalone OTBR against a dedicated RCP (network or local) | ✓ Primary target |
| `silabs-multiprotocol` | Silabs "multiprotocol RCP" — ZHA + OTBR sharing one Silabs radio via CPCd | ✗ Different architecture (CPCd/multiplex layer above HDLC). Not applicable to a dual-radio MR4U. |

We only care about `openthread_border_router`. `silabs-multiprotocol` uses the Silabs CPCd multiplex daemon on top of HDLC, which is a whole different topology — it's designed for a single Silabs chip running EmberZNet + OpenThread simultaneously. Our MR4U keeps Zigbee and Thread on physically separate chips, so we take the standalone OTBR path.

### 2.1 `config.yaml` (v3.1.2)

```yaml
image: homeassistant/{arch}-addon-otbr
init: false
options:
  device: null
  baudrate: "460800"
  flow_control: true
  otbr_log_level: notice
  firewall: true
  nat64: false
  beta: false
ports:
  8080/tcp: null       # OpenThread Web port
  8081/tcp: null       # OpenThread REST API port
ports_description:
  8080/tcp: OpenThread Web port
  8081/tcp: OpenThread REST API port
schema:
  device: device(subsystem=tty)
  baudrate: list(57600|115200|230400|460800|921600|1000000)
  flow_control: bool
  backbone_interface: str?
  network_device: str?
  otbr_log_level: list(debug|info|notice|warning|error|critical|alert|emergency)
  firewall: bool
  nat64: bool
  beta: bool?
```

Key points:

- **`device`** takes a value validated by HA supervisor's `device(subsystem=tty)` schema type — this is a real `/dev/tty*` or `/dev/serial/by-id/…`, **not a URL**. So `device: esphome-hass://…` would be rejected at config-load time by the supervisor.
- **`network_device`** is an unvalidated free-form string. The current `socat-otbr-tcp/run` script uses it as `tcp:${network_device}` (i.e. as a `host:port` pair passed to socat). Setting `device: null` + `network_device: <host:port>` is what selects the network-RCP branch.
- **No add-on-level option currently exists to point at an `esphome-hass://` URL.** Adding one is a small YAML schema change plus a branch in the launcher scripts.
- `baudrate` and `flow_control` are applied to both the real UART path (via `otbr-agent`'s query string) and to the network path (mostly ignored, but preserved for `migrate_otbr_settings.py`).

### 2.2 `Dockerfile` (highlights)

Two-stage build: an `otbr-*-builder` stage compiles `ot-br-posix` and OpenThread from the openthread GitHub repo, and a runtime stage assembles the final image. The relevant CMake options for the compiled `otbr-agent` include:

```cmake
-DOT_POSIX_RCP_HDLC_BUS=ON
-DOT_THREAD_VERSION=1.4
-DOT_RCP_RESTORATION_MAX_COUNT=2
-DOTBR_MDNS=openthread
-DOTBR_WEB=ON
-DOTBR_BORDER_ROUTING=ON
-DOTBR_REST=ON
-DOTBR_BACKBONE_ROUTER=ON
-DOTBR_TREL=ON
-DOTBR_NAT64=ON
```

**`OT_POSIX_RCP_HDLC_BUS=ON` is the one that constrains us.** It compiles in only the POSIX-serial HDLC transport. `OT_POSIX_RCP_SPI_BUS` and other transports are off. There is no `OT_POSIX_RCP_ESPHOME_BUS`, of course.

The runtime image installs:

```dockerfile
RUN pip install --break-system-packages --no-cache-dir \
       universal-silabs-flasher==${UNIVERSAL_SILABS_FLASHER_VERSION} \
       serialx==${SERIALX_VERSION}
```

plus native packages: `python3`, `python3-pip`, `netcat-openbsd`, `socat`, `iproute2`, `iputils-ping`, `ipset`, `iptables`, `libreadline8`, `libncurses6`, `libprotobuf-lite32`, `libjsoncpp26`.

**Interpretation:** the base image is designed for exactly the kind of adapter we want to write.
- `serialx` is present → we can call `serialx.serial_for_url("esphome-hass://…")` directly.
- `universal-silabs-flasher` is present → Silabs radio firmware updates can potentially reuse the same transport. Whether the flasher itself opens the URL via `serialx` or via its own path is a Phase 7 detail, not a Phase 0.5 blocker.
- `socat` is present → in principle we could shim the current architecture in a hurry, but architecturally cleaner to replace it.
- `python3-pip` is present → we can add `aioesphomeapi` at build time trivially.

We may need `aioesphomeapi` explicitly. It might already be present as a transitive dep of `universal-silabs-flasher` — needs verification during Phase 4 prototyping. Worst case, one extra `pip install` line in the Dockerfile.

## 3. `otbr-agent` invocation

From `rootfs/etc/s6-overlay/s6-rc.d/otbr-agent/run`:

```bash
device=$(bashio::config 'device')

if bashio::config.has_value 'network_device'; then
    device="/tmp/ttyOTBR"
fi

baudrate=$(bashio::config 'baudrate')
flow_control=""

if bashio::config.true 'flow_control'; then
    flow_control="&uart-flow-control"
    migrate_flow_control="hardware"
else
    flow_control="&uart-init-deassert"
    migrate_flow_control="none"
fi

# ... migrate OTBR settings first ...
python3 /usr/local/bin/migrate_otbr_settings.py \
    --adapter "${device}" \
    --baudrate "${baudrate}" \
    --flow-control "${migrate_flow_control}" \
    --data-dir /data/thread/

bashio::log.info "Starting otbr-agent..."
exec s6-notifyoncheck -d -s 300 -w 300 -n 0 stdbuf -oL \
    "/usr/sbin/otbr-agent" -I ${thread_if} -B "${backbone_if}" \
        --rest-listen-address "${otbr_rest_listen}" \
        -d${otbr_log_level_int} -v -s \
        "spinel+hdlc+uart://${device}?uart-baudrate=${baudrate}${flow_control}" \
        "trel://${backbone_if}"
```

Interpretation:

- **`otbr-agent` receives exactly one URL scheme for the radio: `spinel+hdlc+uart://<posix-path>?…`.** Query-string options are OpenThread-specific: `uart-baudrate=<N>`, plus one of `uart-flow-control` (enable HW flow control on the PTY) or `uart-init-deassert` (a startup convention).
- When `network_device` is set, `device="/tmp/ttyOTBR"` → `otbr-agent` opens a PTY that some **other** s6 service is expected to be maintaining. This decoupling is what makes the socat → PTY replacement viable: `otbr-agent` doesn't care who's on the other end of the PTY, only that the file descriptor behaves like a UART.
- The `s6-notifyoncheck` wrapper enforces a startup readiness check (via `data/check`, which tests for `/run/openthread-wpan0.sock` and the REST API TCP port). If `otbr-agent` doesn't reach ready state in 300 s, s6 restarts the service.
- The `migrate_otbr_settings.py` step is a one-time schema migration for the OTBR settings database. It runs pre-boot with the same `--adapter` path. If the path is `/tmp/ttyOTBR`, the PTY must already exist and be readable by the time this migration runs — implying a service ordering constraint (socat before migrate before otbr-agent). This will matter for our replacement adapter too.

## 4. Current PTY / socat architecture

From `rootfs/etc/s6-overlay/s6-rc.d/socat-otbr-tcp/run`:

```bash
#!/usr/bin/with-contenv bashio
network_device=$(bashio::config 'network_device')

bashio::log.info "Starting socat TCP client for OTBR daemon..."
exec s6-notifyoncheck -d -s 300 -w 300 \
    "/usr/bin/socat" -d pty,raw,echo=0,link=/tmp/ttyOTBR,ignoreeof \
    "tcp:${network_device}"
```

Interpretation:

- **`socat` opens a PTY** (pty side, raw, no echo), symlinks it to `/tmp/ttyOTBR`, and connects the master side to `tcp:${network_device}` (a plain TCP client connection to `host:port`).
- **This is the plaintext channel the design targets for elimination.** Anyone on the LAN who can reach `network_device:${port}` gets the raw Spinel/HDLC byte stream — no auth, no encryption. This exact pattern is what `SLZB-OS` currently ships (port 7638 = EFR32 stream_server).
- `socat` is invoked with `-d` (single-`-d` = warning-only log level), `pty,raw,echo=0,link=/tmp/ttyOTBR,ignoreeof`, and `tcp:<host>:<port>`. No retries, no reconnect. If the TCP connection drops, socat exits, s6 restarts it, `otbr-agent` sees a PTY hang-up and restarts.
- **The whole service reads as ~10 lines of shell wrapping one `socat` invocation.** Our replacement is architecturally the same size — one s6 service, one Python launcher.

## 5. Replacement plan (Phase 4 concretized)

The design doc §11.1 already sketches this. Phase 0.5 confirms the specifics:

### 5.1 New s6 service `esphome-otbr-uart` (or fork of `socat-otbr-tcp`)

Approximate shape:

```
rootfs/etc/s6-overlay/s6-rc.d/esphome-otbr-uart/
    type          # oneshot or longrun
    up            # invokes /usr/local/bin/esphome-otbr-uart-adapter
    dependencies.d/base
```

And a Python script `esphome-otbr-uart-adapter`:

```py
import asyncio, os, pty
import serialx
# aioesphomeapi is used transitively via serialx's esphome-hass:// handler

async def main():
    # 1. Read config: URL (esphome-hass://…), symlink target (/tmp/ttyOTBR),
    #    baudrate (from add-on config), maybe flow_control.
    url = os.environ["ESPHOME_OTBR_URL"]
    link = os.environ.get("ESPHOME_OTBR_LINK", "/tmp/ttyOTBR")
    baud = int(os.environ.get("ESPHOME_OTBR_BAUDRATE", "460800"))

    # 2. Open PTY. `os.openpty()` returns (master_fd, slave_fd) and the slave
    #    path is os.ttyname(slave_fd). Symlink the slave path to /tmp/ttyOTBR.
    master_fd, slave_fd = pty.openpty()
    os.symlink(os.ttyname(slave_fd), link)

    # 3. Open the ESPHome side via serialx. This uses aioesphomeapi under the
    #    hood and rides the encrypted Native API (Phase 0.3 §5).
    esp_reader, esp_writer = await serialx.open_serial_connection(url, baudrate=baud)

    # 4. Two forwarding tasks: master_fd <-> esp_reader/writer.
    await asyncio.gather(
        forward_pty_to_esp(master_fd, esp_writer),
        forward_esp_to_pty(esp_reader, master_fd),
    )

asyncio.run(main())
```

(The exact `serialx` API surface for standalone opens is a Phase 4 detail — it might be `serialx.serial_for_url()` for the sync path or `open_serial_connection` for the asyncio path. Confirmed to exist per Phase 0.3.)

### 5.2 Config surface

Simplest option: add a **new** add-on config field, keeping backward compat.

```yaml
schema:
  device: device(subsystem=tty)
  network_device: str?
  esphome_device: str?              # NEW — an esphome-hass:// URL
  esphome_psk: password?            # NEW — Noise PSK for the ESPHome device
  # ... rest unchanged
```

Then in `otbr-agent/run`:

```bash
if bashio::config.has_value 'esphome_device'; then
    device="/tmp/ttyOTBR"
    # esphome-otbr-uart service was started earlier by s6 (with ordering dep);
    # it maintains /tmp/ttyOTBR as long as this service is up
elif bashio::config.has_value 'network_device'; then
    device="/tmp/ttyOTBR"
    # legacy socat-otbr-tcp path
else
    device=$(bashio::config 'device')
fi
```

Three mutually-exclusive branches (real device / plaintext TCP / esphome-hass). Legacy behaviour preserved.

Alternative (probably cleaner in the long run): change `network_device` to accept URL schemes and branch inside `socat-otbr-tcp/run` based on scheme prefix. But this changes existing behavior semantics; not friendly to users on `network_device: 192.168.1.5:7638` today.

### 5.3 Service ordering

s6 dependencies inside the add-on look approximately like:

```
socat-otbr-tcp  ─┐
esphome-otbr-uart ─┼── otbr-agent-configure ── otbr-agent ── otbr-web / …
                 ─┘
```

Only one of the two "PTY producer" services is active per config. `otbr-agent-configure` (and `migrate_otbr_settings.py`) both need the PTY link at `/tmp/ttyOTBR` to be readable, so both must depend on whichever PTY producer is up. Ordering in the s6-overlay `dependencies.d/` directories will need patching.

### 5.4 Failure semantics

Preserve the existing `s6-notifyoncheck -d -s 300 -w 300` supervision pattern:

- If ESPHome API disconnect (device reboot / Wi-Fi glitch / API session refresh), the adapter exits with a nonzero code.
- s6 restarts it (with the standard backoff).
- Restart re-creates the PTY. `otbr-agent` sees hang-up on its side, s6 detects otbr-agent's readiness check failing, restarts it too.
- End result: transient link glitches convert cleanly to a coordinated OTBR bounce, same as socat does today.

This behavior is what the design doc §11.3 calls "supervised-failure lifecycle." Confirmed to be idiomatic within the add-on's supervision framework.

## 6. What Phase 0.5 did NOT verify

1. **Whether `aioesphomeapi` is a transitive dep of `universal-silabs-flasher`.** If yes, we get it for free. If no, we add one line to the Dockerfile. Cheap to resolve in Phase 4.
2. **Exact `serialx` API for a standalone open** (not through `zigpy.serial`). Phase 0.3 confirmed `serialx.platforms.serial_esphome.ESPHomeSerial` exists; the standalone user-facing helper is likely `serialx.serial_for_url(url)` for sync or `serialx.open_serial_connection(url, ...)` for asyncio. Concrete API validation belongs in Phase 4.
3. **Whether `universal-silabs-flasher` can be pointed at an `esphome-hass://` URL for EFR32 firmware updates.** Relevant to Phase 7 (radio flashing), not Phase 0.5. Given serialx + esphome are both already in the runtime image, likely yes.
4. **How OpenThread reacts to the PTY temporarily disappearing.** The current socat path has the same reconnect story we'd inherit; empirical Phase 5 concern.
5. **s6 service ordering DSL specifics.** The Phase 4 prototype will need to author `dependencies.d/` files correctly. This is a mechanical detail, not a design blocker.

## 7. Implications for the design doc

| Finding | Affects design doc section | Nature of change |
|---|---|---|
| `serialx` is already installed in the OTBR runtime image | Phase 4 checklist | Downgrade: "add `serialx[esphome]`" becomes "confirm `serialx` present (it is)". Only `aioesphomeapi` may need adding. |
| `otbr-agent` requires `spinel+hdlc+uart://<posix-path>`; no URL for network RCP | §11 (OTBR add-on architecture) | Confirm as no longer speculative; the PTY-bridge approach is the only viable option |
| Current `socat-otbr-tcp` service is 5 lines of shell wrapping one socat invocation | §11 (replacement scope) | Concretize: our replacement is ~1 s6 service + ~50-line Python script |
| `network_device` config is free-form; `device` is validated as `device(subsystem=tty)` | Phase 4 config surface | Add: a new `esphome_device` option keeps backward-compat cleanly |
| Two PTY-producer services must be mutually exclusive via s6 ordering | Phase 4 checklist | Add: service ordering step |
| Migration script `migrate_otbr_settings.py` runs pre-boot with `--adapter` | Phase 4 startup ordering | Note: PTY producer must reach ready state before migration, before otbr-agent |
| `silabs-multiprotocol` add-on is a different architecture (CPCd/multiplex) | §11 (scope), §9 (native ZHA) | Note: irrelevant to dual-chip MR4U; standalone OTBR path is the only one we target |

## 8. Update to project risk profile

Before Phase 0.5, the OTBR replacement was the most opaque piece of the design — a black box with a "we'll figure out an add-on-internal adapter" placeholder. **After Phase 0.5, the shape is fully specified**: one new s6 service, one small Python script using pre-installed `serialx`, one new config field, a small ordering-file change, all self-contained inside the OTBR add-on. The Phase 4 engineering task is now well-defined.

Two risks upgraded from unknown to bounded-and-known:

- **API surface**: we still don't know the exact `serialx` open-a-URL helper name for standalone use. Bounded — 5 minutes of dev-container work in Phase 4.
- **Upstreamability**: whether HA maintainers accept an `esphome_device` config field in the OTBR add-on. If not, we ship a fork of the add-on. Either way, no design blocker.

**Phase 0 is now substantively complete.** All five upstream inspections are done. The design has a defensible ground-truth foundation for every architectural claim in §7–§11.
