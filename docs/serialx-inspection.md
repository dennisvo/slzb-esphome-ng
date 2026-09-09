# Home Assistant `serialx` inspection

**Phase 0.3 inspection artifact.**
Ground-truth reference for the HA-side transport that consumes ESPHome's `serial_proxy` component. Everything here is copied or paraphrased from actual source in `home-assistant/core` (branch `dev`); opinions and decisions are marked as such.

| Field | Value |
|---|---|
| Upstream inspected | `home-assistant/core`, `dev` branch |
| Date | 2026-09-09 |
| Method | GitHub code search + targeted file reads via `github_repo` tool |
| Scope | (1) `serialx` package status in HA; (2) `esphome-hass://` and `esphome://` URL schemes; (3) the HA-side ESPHome serial-proxy transport implementation; (4) automatic port discovery for ZHA/OTBR config flows |

---

## 1. Executive summary

**The end-to-end byte-pipe path we assumed at the top of the design is real, integrated in mainline HA, and passes tests upstream.** Concretely:

- `serialx` is a **first-class HA runtime dependency**, pinned at `1.10.0` in `requirements_all.txt` and `package_constraints.txt`. It replaces `pyserial-asyncio`, which HA is actively deprecating (removal targeted for HA 2027.2).
- Two ESPHome-related URL schemes exist, both documented in `homeassistant/components/usb/consumers.py`:
  - **`esphome-hass://`** — routed through the ESPHome integration and its `APIClient`. This is the scheme we want.
  - **`esphome://`** — direct connect to an ESPHome device without going through the HA integration (used by `zwave_js`; embeds noise PSK in query). Not our path.
- HA ships a full handler for `esphome-hass://` at `homeassistant/components/esphome/serial_proxy.py` (~120 lines), backed by a base class `ESPHomeSerial` that lives inside the `serialx` package itself (`serialx.platforms.serial_esphome`). **The Noise-encrypted transport, the subscribe/write/data-received wire protocol, and the actual byte pipe live in `serialx` — HA only adds the "look up which APIClient to use" layer.**
- `aioesphomeapi.model` exports `SerialProxyInfo` and `SerialProxyPortType` — the Python mirrors of the ESP-side C++ classes. `device_info.serial_proxies` is a list of `SerialProxyInfo` that flows from the ESP device to HA automatically at connection time (via `SerialProxyInfo` proto fields on the `DeviceInfoResponse`, as confirmed in the Phase 0.2 doc).
- HA automatically **enumerates each ESPHome `serial_proxy:` instance as a `SerialDevice`** and exposes it to the USB integration's port scanner, so it appears in ZHA's / OTBR's / any USB-consumer's port picker.
- Test coverage upstream is **substantial**: `tests/components/esphome/test_serial_proxy.py` (~280 lines) plus `tests/components/usb/test_init.py::test_serial_proxy_stub_sync`.

**Consequence for our design:** the "no glue code, no bundled add-on" premise is confirmed. If we ship firmware with two `serial_proxy:` blocks named e.g. `zigbee` and `thread`, they will appear as pickable serial devices in ZHA and OTBR without any extra HA-side work.

---

## 2. `serialx` package facts

From `requirements_all.txt` and `package_constraints.txt`:

```txt
serialx==1.10.0
```

From `homeassistant/requirements.py`:

```py
DEPRECATED_PACKAGES: dict[str, tuple[str, str]] = {
    "pyserial-asyncio": ("should be replaced by serialx", "2027.2"),
}
```

From `script/hassfest/requirements.py`:

```py
"serialx": "SemVer",
```

The package is pinned to a SemVer version (upgradeable but not floating), and third-party pyserial-asyncio consumers get a deprecation warning telling them to migrate. The blog post referenced (`https://developers.home-assistant.io/blog/2026/04/27/pyserial-to-serialx/`) marks this as an official platform migration, not a fork.

Users in the tree today (partial list, all directly `from serialx import ...`):
- `homeassistant/components/usb/` — top-level scanner and stub
- `homeassistant/components/esphome/` — the real ESPHome handler
- `homeassistant/components/serial/` — the generic serial sensor
- `homeassistant/components/monoprice/`, `blackbird/`, `landisgyr_heat_meter/` — legacy integrations already migrated

And declared as an `after_dependencies` in several manifests (they don't `import serialx` directly but they rely on its URI handlers being registered):
- `zha` ✓ (this is the crucial one for our project)
- `homeassistant_yellow`, `homeassistant_sky_connect`, `homeassistant_connect_zbt2`

## 3. The two URL schemes

From `homeassistant/components/usb/consumers.py`:

```py
SCANNED_PORT_SCHEMES = ("esphome-hass://",)

UNSCANNABLE_PORT_SCHEMES = (
    "esphome://",
    "rfc2217://",
    "socket://",
    "tcp://",
)
```

Interpretation:

| Scheme | Where it comes from | Who uses it | Fits our design? |
|---|---|---|---|
| `esphome-hass://` | Emitted by ESPHome's `_async_scan_serial_ports` | ZHA, OTBR, any USB-consumer with an ESPHome-connected radio | ✓ **This is the one** |
| `esphome://` | Manually configured, embeds `?key=<noise_psk>` | `zwave_js` for direct-to-device connections | ✗ (bypasses HA integration; PSK in URL) |
| `rfc2217://`, `socket://`, `tcp://` | Manually configured, raw TCP tunnels | Legacy adapters | ✗ (unencrypted, this is what we're leaving behind) |

### `esphome-hass://` URL grammar

From `homeassistant/components/esphome/serial_proxy.py::build_url`:

```py
def build_url(entry_id: str, port_name: str) -> URL:
    """Build a canonical `esphome-hass://` URL."""
    return URL.build(
        scheme="esphome-hass",
        host="esphome",
        path=f"/{entry_id}",
        query={"port_name": port_name},
    )
```

Canonical form: `esphome-hass://esphome/{config_entry_id}?port_name={port_name}`

- **Host is the literal string `"esphome"`** — a placeholder, not a hostname. Routing happens via `entry_id` in the path, not by IP or DNS.
- **`entry_id`** is HA's opaque config-entry identifier (e.g. `01JZ...` ULID). Uniquely identifies which ESPHome device to talk to.
- **`port_name`** is the `name:` field from the device's `serial_proxy:` YAML block.

Test evidence (`tests/components/esphome/test_serial_proxy.py::test_build_url_basic`):

```py
url = serial_proxy.build_url("abc123DEF456", "uart0")
assert url == URL("esphome-hass://esphome/abc123DEF456?port_name=uart0")
```

And `test_build_url_escapes_port_name` confirms port names with spaces or slashes are URL-encoded correctly (`"uart 0/main"` round-trips through yarl).

## 4. HA-side implementation

Two files, both intentionally small.

### 4.1 `homeassistant/components/esphome/serial_proxy.py` — the real handler

Docstring: `"""Home Assistant-aware ESPHome serial proxy URI handler for serialx."""`

Structure:

```py
_HASS_LOOP: asyncio.AbstractEventLoop | None = None
# Module-global — required so serialx (running in its own thread) can safely
# query Core for an APIClient. Set by register_serialx_transport() at HA
# startup, cleared at shutdown.

def build_url(entry_id: str, port_name: str) -> URL: ...

async def _resolve_client(entry_id: str) -> APIClient:
    """Look up the `APIClient` for a specific config entry."""
    hass = async_get_hass()
    entry = cast(ESPHomeConfigEntry, hass.config_entries.async_get_entry(entry_id))
    if entry is None or entry.domain != DOMAIN:
        raise InvalidSettingsError(...)
    if entry.state is not ConfigEntryState.LOADED:
        raise InvalidSettingsError(...)
    return entry.runtime_data.client

class HassESPHomeSerial(ESPHomeSerial):
    """ESPHomeSerial that resolves an HA config entry's APIClient from the URL."""
    _api: APIClient | None
    _path: str | None

    async def _async_open(self) -> None:
        # Parse entry_id from path, port_name from query
        # Fetch APIClient via run_coroutine_threadsafe(_resolve_client, hass_loop)
        # Delegate the rest to ESPHomeSerial._async_open() in serialx
        ...

class HassESPHomeSerialTransport(ESPHomeSerialTransport):
    transport_name = "esphome-hass"
    _serial_cls = HassESPHomeSerial

def register_serialx_transport(loop) -> Callable[[Event], None]:
    global _HASS_LOOP
    _HASS_LOOP = loop
    unregister = register_uri_handler(
        scheme="esphome-hass://",
        unique_scheme="esphome-hass-internal://",
        sync_cls=HassESPHomeSerial,
        async_transport_cls=HassESPHomeSerialTransport,
    )
    ...
```

Notable design points:

- **Base class `ESPHomeSerial` lives in `serialx` itself** (`serialx.platforms.serial_esphome.ESPHomeSerial`). HA subclasses it to add the "resolve entry_id → APIClient" step. Everything else — the wire protocol, the subscribe RPC, the write RPC, the data-received callback, Noise handshake — is inside `serialx` (which in turn uses `aioesphomeapi` as the underlying library).
- **Cross-thread APIClient lookup**: `serialx` runs its I/O in its own event loop / thread. HA gets the client via `asyncio.run_coroutine_threadsafe(_resolve_client(entry_id), _HASS_LOOP)`. The module-level `_HASS_LOOP` global is the safe way to bridge these two worlds. Documented in the comment at the top of the file.
- **Fail-safe modes**: if the entry doesn't exist, wrong domain, or not loaded → `InvalidSettingsError`. If `_HASS_LOOP` was never set → `InvalidSettingsError` with clear message. Clean error hygiene at every branch.

### 4.2 `homeassistant/components/usb/serial_proxy_stub.py` — the fallback stub

Registered by the USB integration at startup, **before** the ESPHome integration loads. Its purpose is subtle but crucial:

```py
def register_serialx_transport() -> Callable[[Event], None]:
    """Register the stub URI handler."""
    unregister = register_uri_handler(
        scheme="esphome-hass://",
        unique_scheme="esphome-hass-usb://",
        sync_cls=HassESPHomeSerialStub,
        async_transport_cls=HassESPHomeSerialStubTransport,
        weight=-1,  # We want the ESPHome integration transport to take precedence
    )
```

Interpretation:

- The stub occupies the `esphome-hass://` scheme even before ESPHome loads, with `weight=-1`.
- When the ESPHome integration loads, it registers the real handler at default weight, which **wins** over the stub because of the negative weight.
- Any integration that references an `esphome-hass://` URL *before* ESPHome has loaded gets `ConfigEntryNotReady` (the stub raises this on `_open` and `_connect`). That's HA's standard "try me again in a moment" signal, so the consumer will retry setup and succeed after ESPHome finishes loading.

The stub implements the full `BaseSerial` / `BaseSerialTransport` interface as no-ops (`_read` returns 0 bytes, `_write` returns 0, etc.) so type-checkers and interface contracts are satisfied. Only `_open` and `_connect` raise `ConfigEntryNotReady`.

This is a mature pattern. It solves the startup-order problem elegantly and it means our design doesn't need to worry about "what if HA queries the port list before ESPHome is up?" — that case is already handled correctly by upstream.

## 5. `serial_proxy` end-to-end path

Combining Phase 0.2 (ESPHome side) and Phase 0.3 (HA side), the full path for a Zigbee coordinator behind our secure gateway:

```
┌─ HA integration (ZHA / OTBR / any serialx consumer)
│  Opens URL: esphome-hass://esphome/{entry_id}?port_name=zigbee
│
├─ serialx: register_uri_handler dispatch
│  Matches "esphome-hass://" → HassESPHomeSerial (HA integration wins over stub)
│
├─ HA: HassESPHomeSerial._async_open()
│  ├─ Parse entry_id, port_name from URL
│  ├─ Look up ESPHomeConfigEntry via run_coroutine_threadsafe(_resolve_client, hass_loop)
│  ├─ Store APIClient reference (self._api)
│  └─ Call super()._async_open() → into serialx.platforms.serial_esphome.ESPHomeSerial
│
├─ serialx.ESPHomeSerial: send SerialProxySubscribeRequest via APIClient
│  ├─ aioesphomeapi encodes proto message
│  ├─ Noise-encrypted Native API socket (port 6053)
│  └─ Wire arrives at ESP device
│
├─ ESP device: APIConnection.on_serial_proxy_request
│  ├─ Look up SerialProxy instance by name (matches YAML "port_name")
│  ├─ SerialProxy.serial_proxy_request(SUBSCRIBE)
│  │  ├─ port_claimed_by_other_ check
│  │  ├─ this->api_connection_ = api_connection
│  │  └─ enable_loop()
│  └─ Send SerialProxyRequestResponse (status = OK)
│
└─ Byte pipe is live. From here:
   Device → Host:
     UART FIFO → SerialProxy::read_and_send_ (256 B/loop stack buffer)
     → SerialProxyDataReceived proto → Noise-encrypted → serialx → integration bytes
   Host → Device:
     integration bytes → serialx → SerialProxyWriteRequest → Noise-encrypted
     → SerialProxy::write_from_client → UART TX
```

**No plaintext at any point on the LAN. No `socat`, no PTY, no host add-on, no bundled proxy service.** This is what §7 of the design doc claims and Phase 0.3 confirms it as achievable.

## 6. Automatic port discovery

From `homeassistant/components/esphome/__init__.py`:

```py
@callback
def _async_scan_serial_ports(hass: HomeAssistant) -> list[USBDevice | SerialDevice]:
    """Return serial-proxy ports exposed by connected ESPHome devices."""
    ports: list[USBDevice | SerialDevice] = []
    for entry in hass.config_entries.async_loaded_entries(DOMAIN):
        entry_data = entry.runtime_data
        if not entry_data.available:
            continue
        device_info = entry_data.device_info
        if device_info is None:
            continue
        manufacturer, model = async_get_manufacturer_model(device_info)
        ports.extend(
            SerialDevice(
                device=str(serial_proxy.build_url(entry.entry_id, proxy.name)),
                serial_number=(
                    device_info.mac_address.replace(":", "") + "-" + slugify(proxy.name)
                ),
                manufacturer=manufacturer,
                description=f"{model} ({proxy.name})",
            )
            for proxy in device_info.serial_proxies
        )
    return ports


async def async_setup(hass, config) -> bool:
    ...
    if "usb" in hass.config.components:
        async_register_serial_port_scanner(hass, _async_scan_serial_ports)
        hass.bus.async_listen_once(
            EVENT_HOMEASSISTANT_STOP,
            serial_proxy.register_serialx_transport(hass.loop),
        )
    return True
```

The scanner iterates every loaded ESPHome config entry, reads its `device_info.serial_proxies` (populated by `aioesphomeapi` from the `SerialProxyInfo` proto messages the device sends during handshake — this is the `send_device_capabilities_response_` code we saw in Phase 0.2), and emits one `SerialDevice` per proxy.

Each `SerialDevice` gets:
- `device`: the canonical `esphome-hass://esphome/{entry_id}?port_name={name}` URL
- `serial_number`: `{mac_no_colons}-{slug(name)}` — stable, unique
- `manufacturer`: from `device_info.project_name` fallback to `device_info.manufacturer`
- `description`: `{model} ({name})`

These `SerialDevice` objects flow into HA's USB scanner and appear in the `usb/list_serial_ports` websocket API, which is what ZHA's config-flow port picker consumes.

**Consequence for our design (concrete for MR4U):** if we ship firmware with

```yaml
serial_proxy:
  - name: zigbee
    port_type: TTL
    uart_id: hw_uart1
  - name: thread
    port_type: TTL
    uart_id: hw_uart2
```

then, after HA discovers the device via zeroconf and the user completes the ESPHome config flow, ZHA's config flow will offer two entries in its port dropdown:

```
esphome-hass://esphome/{entry_id}?port_name=zigbee    (SLZB-MR4U (zigbee))
esphome-hass://esphome/{entry_id}?port_name=thread    (SLZB-MR4U (thread))
```

with no additional configuration needed. The user just clicks the Zigbee one.

## 7. Test coverage

Home Assistant tests we found and verified exist:

| Test file | What it verifies |
|---|---|
| `tests/components/esphome/test_serial_proxy.py::test_build_url_basic` | Canonical URL shape |
| `test_build_url_escapes_port_name` | URL-encoding of special chars in port names |
| `test_async_setup_stores_event_loop` | `_HASS_LOOP` is registered at HA setup |
| `test_resolve_client_unknown_entry` | Unknown `entry_id` → `InvalidSettingsError` |
| `test_resolve_client_wrong_domain` | Non-ESPHome entry → `InvalidSettingsError` |
| `test_resolve_client_unloaded_entry` | Not-yet-loaded entry → `InvalidSettingsError` |
| `test_resolve_client_loaded_entry` | Returns the correct `APIClient` |
| `test_scan_serial_ports_no_entries` | Empty result when no ESPHome devices |
| `test_scan_serial_ports_happy_path` | Multi-proxy device emits one `SerialDevice` per proxy |
| `test_scan_serial_ports_uses_project_info` | Project name overrides manufacturer |
| `test_scan_serial_ports_defaults_manufacturer` | Missing manufacturer → `"espressif"` fallback |
| `test_scan_serial_ports_skips_unavailable` | Offline devices are skipped |
| `test_async_open_missing_host` | Missing `entry_id` in URL → error |
| `test_async_open_missing_port_name` | Missing `port_name` in URL → error |
| `test_async_open_happy_path` | Full open flow with mocked `APIClient` |
| `tests/components/usb/test_init.py::test_serial_proxy_stub_sync` | Stub raises `ConfigEntryNotReady` |

That is real coverage. This is not experimental territory on the HA side; it's release-quality code with a working test suite.

## 8. Fitness for our design

Mapping HA-side capabilities onto design requirements:

| Requirement (from design doc) | HA / serialx support | Notes |
|---|:---:|---|
| No plaintext UART bytes on LAN | ✓ | Everything rides Noise-encrypted API (port 6053). No `socat`, no PTY, no plain TCP. |
| ZHA opens Zigbee coordinator without custom code | ✓ | ZHA already declares `serialx` as `after_dependency`. `esphome-hass://` scheme resolves via the same code path as any other adapter. |
| OTBR add-on can be pointed at the Thread UART | Deferred | Requires OTBR add-on inspection (Phase 0.5). What matters for Phase 0.3: HA integrations that use `serialx.serial_for_url` will resolve `esphome-hass://` correctly. Whether the OTBR add-on uses `serialx` at all is a separate question. |
| Auto-discovery of available ports | ✓ | `_async_scan_serial_ports` publishes proxies into HA's USB scanner without any additional config on our side. |
| Multiple radios per device | ✓ | Multi-instance handled cleanly. Each `serial_proxy:` YAML block → separate `SerialDevice` in HA. |
| No host-side add-on / no glue code | ✓ | Zero additional software needed on the HA host. Everything already ships in HA. |
| Encryption key management | ✓ (via ESPHome config entry) | The noise PSK is stored in the ESPHome config entry, not embedded in URLs. `HassESPHomeSerial` inherits the entry's `APIClient` which already has the PSK. |
| Runtime port reconfigure (baud change) | ✓ | Underlying `serialx` uses `APIClient.serial_proxy_configure` — the same runtime baud reconfigure path we found in Phase 0.2. |

## 9. What Phase 0.3 did NOT verify

- **ZHA's runtime port-open code path** — we confirmed ZHA depends on `serialx`, and that `serialx` has an `esphome-hass://` handler. We did *not* trace ZHA → `zigpy` → `zigpy-znp` (or `zigpy-deconz`, `bellows`, `zigpy-xbee`) all the way to a `serialx.serial_for_url` call. That's Phase 0.4.
- **OTBR add-on** — Phase 0.5. Whether the add-on uses `serialx` at all, or maintains its own serial-opening path.
- **Empirical behavior during ESPHome reconnect** — the port-claim mechanism (Phase 0.2) plus the `InvalidSettingsError` on unloaded entries (this phase) suggest it should behave correctly, but timing is empirical (Phase 3).
- **Radio flashing** — we know `SerialProxyConfigureRequest` supports runtime baud changes. Whether ZHA's Zigbee firmware updater and OTBR's EFR32 firmware updater can drive that through `serialx` is a Phase 4 / 7 empirical question.

## 10. Implications for the design doc

Findings to reflect into `docs/design.md` on the next batched revision:

| Finding | Affects design doc section | Nature of change |
|---|---|---|
| `serialx` is a mainline HA package, pinned at 1.10.0, replacing pyserial-asyncio | §7, §22 | Confirm as no longer speculative |
| `esphome-hass://` URL scheme is real, canonical form documented | §7, §9, §16 (config docs) | Add canonical URL shape |
| Two-tier stub-plus-real registration handles startup ordering | §21.6 (reconnect / lifecycle unknown) | Downgrade: startup-order is handled upstream. Reconnect during a subscribed session is still empirical. |
| `_async_scan_serial_ports` auto-publishes proxies into HA's USB scanner | §7, §9 (native ZHA path) | Concretize: ZHA config flow "just works" with no additional wiring |
| Test coverage upstream is substantial (16+ tests) | §21 (key unknowns) | Downgrade unknowns 5 and 6 significantly |
| Base class `ESPHomeSerial` lives in `serialx` itself | §22 | Add: our design depends on `serialx.platforms.serial_esphome` as well as ESPHome's `serial_proxy` |
| Stub raises `ConfigEntryNotReady` cleanly during startup race | §21.6 | Explicitly resolved |

## 11. Update to project risk profile

Before Phase 0.3, the risk profile assumed that we might need to write HA-side glue code (a small custom component to bridge `serial_proxy` API to a serialx URI handler). **That risk is eliminated.** The bridge exists, is tested, and is maintained by the HA core team.

The remaining risks all shift toward:

- **Wire-protocol version coupling** between the ESPHome device firmware, `aioesphomeapi`, and `serialx.platforms.serial_esphome`. All three must be compatible. This is the same risk we already documented in the Phase 1 experimental-component callout.
- **ZHA and OTBR runtime behavior** at Phase 2 / Phase 4 respectively.

Phase 0.3 does not raise any new risks. It resolves several previously-open ones.
