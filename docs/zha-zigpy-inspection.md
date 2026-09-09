# ZHA → zigpy → radio-backend inspection

**Phase 0.4 inspection artifact.**
The final "is anyone bypassing `serialx` and calling `pyserial` directly?" audit. Ground-truth reference: every hop from ZHA down to the serial-URL dispatcher.

| Field | Value |
|---|---|
| Upstream inspected | `home-assistant/core` (`dev`), `zigpy/zigpy` (`dev`), `zigpy/zigpy-znp` (`dev`), `zigpy/bellows` (`dev`), plus quick scans of `zigpy-deconz`, `zigpy-xbee`, `zigpy-zigate`, `zigpy-cc` |
| Date | 2026-09-09 |
| Method | GitHub code search + `github_repo` deep queries + direct raw-file fetch for `zigpy-znp/uart.py` and `bellows/uart.py` |
| Scope | (1) ZHA's handling of the port URL; (2) `zigpy.serial` as the single serial-open path; (3) each in-tree radio backend's serial-connect code path; (4) DTR/RTS modem-pin control support end-to-end |

---

## 1. Executive summary

**No layer in the ZHA → zigpy → CC2674 or EFR32 chain bypasses `serialx`.** Concretely:

- `zigpy.serial.create_serial_connection` is a thin wrapper around `serialx.create_serial_connection`. It is the *only* serial-open path exposed by `zigpy`. Confirmed in `zigpy/zigpy` `dev` branch.
- `zigpy` pins `serialx>=1.4.0` in its `pyproject.toml`. HA pins `serialx==1.10.0` (Phase 0.3). Both align.
- **`zigpy-znp` (our CC2674 backend)** imports `from zigpy.serial import SerialProtocol, create_serial_connection` and calls `await create_serial_connection(url=port, ...)`. No `pyserial`, no `serial_asyncio` anywhere in `zigpy_znp/uart.py`.
- **`bellows` (our EFR32/EZSP backend)** does `import zigpy.serial` and `await zigpy.serial.create_serial_connection(...)`. Same story.
- **ZHA never inspects the URL.** `radio_manager.py` reads `config_entry.data[CONF_DEVICE][CONF_DEVICE_PATH]` and hands it straight to the zigpy application controller. The string `"esphome-hass://esphome/{entry_id}?port_name=zigbee"` flows through opaque.
- **Modem-pin control (DTR/RTS) is wired end-to-end.** `zigpy-znp` calls `await self._transport.set_modem_pins(dtr=..., rts=...)` for CC26xx bootloader entry. The ESPHome `serial_proxy:` C++ component supports `dtr_pin:` / `rts_pin:` YAML keys and implements the corresponding `SerialProxySetModemPinsRequest` proto. `aioesphomeapi` mirrors the request type. **Radio flashing over the encrypted API is achievable** — provided we wire `dtr_pin:` and `rts_pin:` in the MR4U firmware's CC2674 `serial_proxy:` block.

**One legacy backend does bypass**: `zigpy-cc` (for CC2531/CC2530). Not our concern — it's the deprecated pre-Z-Stack-3 backend. Our CC2674 uses `zigpy-znp`.

**Consequence for our design**: The last major structural risk (a hidden `pyserial.Serial(...)` call in some radio backend that would break `esphome-hass://`) is eliminated. All four zigpy backends we plausibly care about (`zigpy-znp`, `bellows`, `zigpy-deconz`, `zigpy-xbee`) route through `zigpy.serial` → `serialx`.

---

## 2. `zigpy.serial` — the single serial-open path

From `zigpy/serial.py` (`zigpy` `dev` branch):

```py
from serialx import (
    SerialTransport,
    create_serial_connection as serialx_create_serial_connection,
)

async def create_serial_connection(
    loop: asyncio.AbstractEventLoop,
    protocol_factory: Callable[[], asyncio.Protocol],
    url: pathlib.Path | str,
    *,
    baudrate: int = 115200,
    xonxoff: bool | UndefinedType = UNDEFINED,
    rtscts: bool | UndefinedType = UNDEFINED,
    flow_control: Literal["hardware", "software"] | None | UndefinedType = UNDEFINED,
    **kwargs: Any,
) -> tuple[asyncio.Transport, asyncio.Protocol]:
    """Wrapper for serialx that provides simplified flow control kwargs."""
    # ... normalise flow control ...
    transport, protocol = await serialx_create_serial_connection(
        loop, protocol_factory,
        url=url, baudrate=baudrate,
        xonxoff=xonxoff, rtscts=rtscts,
        **kwargs,
    )
    return transport, protocol
```

And from `zigpy/pyproject.toml`:

```toml
dependencies = [
    "cryptography",
    "voluptuous",
    "jsonschema",
    "serialx>=1.4.0",
    ...
]
```

`zigpy` has zero `pyserial` or `pyserial-asyncio` imports. The compat layer for the flow-control kwargs is the only value-add over calling `serialx` directly.

## 3. Backend serial-connect audit

One-shot audit of every zigpy backend under `zigpy/*` on GitHub. The question: does `<backend>/uart.py` route through `zigpy.serial`?

| Backend | Radio family | `uart.py` opens via | Verdict |
|---|---|---|---|
| **`zigpy-znp`** | CC2652 / CC2674 (Z-Stack 3) | `from zigpy.serial import SerialProtocol, create_serial_connection` | ✓ **serialx path** |
| **`bellows`** | EFR32 / EZSP (EmberZNet) | `import zigpy.serial; await zigpy.serial.create_serial_connection(...)` | ✓ **serialx path** |
| `zigpy-deconz` | deCONZ ConBee/RaspBee | `await zigpy.serial.create_serial_connection(...)` | ✓ serialx path |
| `zigpy-xbee` | XBee | `await zigpy.serial.create_serial_connection(...)` | ✓ serialx path |
| `zigpy-zigate` | ZiGate | `from zigpy.serial import SerialProtocol, create_serial_connection` | ✓ serialx path |
| **`zigpy-cc`** | CC2531 / CC2530 (legacy Z-Stack pre-3) | `await serial_asyncio.create_serial_connection(...)` | ✗ **bypasses serialx** |

Only `zigpy-cc` bypasses. `zigpy-cc` is the deprecated pre-Z-Stack-3 backend for CC2531/CC2530 dongles; it is **not** the driver for CC2652/CC2674. Our CC2674 radio uses `zigpy-znp`, which is compliant. `zigpy-cc` is irrelevant to this design and doesn't need to be fixed on our critical path.

### 3.1 `zigpy-znp` (our primary CC2674 backend) — full detail

From `zigpy_znp/uart.py`:

```py
from zigpy.serial import SerialProtocol, create_serial_connection

class ZnpMtProtocol(SerialProtocol):
    ...
    async def set_dtr_rts(self, *, dtr: bool, rts: bool) -> None:
        LOGGER.debug("Setting serial pin states: DTR=%s, RTS=%s", dtr, rts)
        await self._transport.set_modem_pins(dtr=dtr, rts=rts)


async def connect(config: conf.ConfigType, api) -> ZnpMtProtocol:
    port = config[zigpy.config.CONF_DEVICE_PATH]

    _, protocol = await create_serial_connection(
        loop=asyncio.get_running_loop(),
        protocol_factory=lambda: ZnpMtProtocol(api, url=port),
        url=port,
        baudrate=config[zigpy.config.CONF_DEVICE_BAUDRATE],
        flow_control=config[zigpy.config.CONF_DEVICE_FLOW_CONTROL],
    )

    await protocol.wait_until_connected()
    return protocol
```

Key observations:

- `port` (from `CONF_DEVICE_PATH`) is treated as opaque. An `esphome-hass://...` URL flows through unchanged.
- `create_serial_connection(url=port, ...)` is the `zigpy.serial` wrapper we saw in §2 → `serialx`.
- `set_dtr_rts` uses `SerialTransport.set_modem_pins(dtr, rts)`. This method exists on `serialx.SerialTransport` and is implemented by every transport that supports modem pins, including `ESPHomeSerialTransport` (via `SerialProxySetModemPinsRequest`, §5).
- The specific pattern for CC26xx bootloader entry is documented in the TI TN and involves toggling `nRESET` (via DTR) while holding `BSL_ENTRY` (via RTS). Because `set_dtr_rts` goes through `set_modem_pins` and that maps to the encrypted `SerialProxySetModemPinsRequest`, **flashing the CC2674 through the ESPHome-tunnelled UART works with no glue code**, provided the ESPHome firmware wires the pins.

### 3.2 `bellows` (EFR32/EZSP) — full detail

From `bellows/uart.py`:

```py
import zigpy.config
import zigpy.serial
from bellows.ash import AshProtocol
from bellows.thread import EventLoopThread, ThreadsafeProxy
import bellows.types as t

class Gateway(zigpy.serial.SerialProtocol):
    ...

async def _connect(config, api):
    ...
    if config[zigpy.config.CONF_DEVICE_FLOW_CONTROL] is None:
        xon_xoff, rtscts = True, False
    else:
        xon_xoff, rtscts = False, True

    transport, _ = await zigpy.serial.create_serial_connection(
        loop,
        lambda: protocol,
        url=config[zigpy.config.CONF_DEVICE_PATH],
        baudrate=config[zigpy.config.CONF_DEVICE_BAUDRATE],
        xonxoff=xon_xoff,
        rtscts=rtscts,
    )
    ...
```

Two notes:

- **`bellows` runs its serial I/O in a dedicated thread** (via `EventLoopThread`, `ThreadsafeProxy`). This is a well-known bellows design choice to avoid blocking the main HA loop with EZSP ACK-timing. It doesn't affect our design — the point is that whichever loop it runs in, it uses `zigpy.serial.create_serial_connection`, which uses `serialx`, which dispatches by URL scheme. `esphome-hass://` still routes through `HassESPHomeSerial`.
- **Flow-control default is quirky**: `bellows` defaults to *software* flow control (`xon_xoff=True`) if `CONF_DEVICE_FLOW_CONTROL` is unset. HA's ZHA config flow for EFR32 typically sets `flow_control="hardware"`, giving `rtscts=True`. Neither is honored by the ESPHome-tunnelled path (there is no local UART), but the ESPHome-side `serial_proxy` CONFIGURE request rejects HW flow control with `SERIAL_PROXY_RESULT_NOT_SUPPORTED` (Phase 0.2 finding). Whether `serialx` for `esphome-hass://` silently swallows the `rtscts=True` kwarg or forwards it as a CONFIGURE request that then errors, is a Phase 2 empirical validation. See §7 open questions.

## 4. ZHA never inspects the URL

From `homeassistant/components/zha/radio_manager.py`:

```py
mgr = cls()
mgr.hass = hass
mgr.device_path = config_entry.data[CONF_DEVICE][CONF_DEVICE_PATH]
mgr.device_settings = config_entry.data[CONF_DEVICE]
mgr.radio_type = RadioType[config_entry.data[CONF_RADIO_TYPE]]
```

And from `homeassistant/components/zha/helpers.py`:

```py
app_config[CONF_DATABASE] = database
app_config[CONF_DEVICE] = ha_zha_data.config_entry.data[CONF_DEVICE]
```

`CONF_DEVICE_PATH` is copied straight from the config entry (which came straight from the config flow's port picker, populated by ESPHome's `_async_scan_serial_ports` — see Phase 0.3) into the `app_config` dict passed to the zigpy backend controller. ZHA never parses, validates, or rewrites the URL. **This is exactly the behaviour we want** — it means `esphome-hass://` has no ZHA-specific plumbing to worry about.

## 5. Modem-pin control end-to-end

Radio flashing (Zigbee OTA of the coordinator firmware; CC26xx bootloader entry) requires toggling DTR and RTS on the host side, translated to actual GPIO pin transitions on the CC26xx `nRESET` and `BSL_ENTRY` lines. The full path:

```
zigpy-znp: await self._transport.set_modem_pins(dtr=False, rts=True)   # Enter BSL
    ↓
serialx.ESPHomeSerialTransport.set_modem_pins()
    ↓
aioesphomeapi.APIClient.send SerialProxySetModemPinsRequest
    ↓
Noise-encrypted API socket (:6053)
    ↓
ESPHome device: SerialProxy receives request
    ↓
GPIO pin toggles: dtr_pin_->digital_write(...), rts_pin_->digital_write(...)
    ↓
Physical wires to CC2674 nRESET / BSL_ENTRY pins
```

Evidence for each hop:

- **`zigpy-znp` side**: `zigpy_znp/uart.py::set_dtr_rts` (shown in §3.1).
- **Proto message** (`esphome/components/api/api.proto` and mirror in `aioesphomeapi/api.proto`):
  ```proto
  // Set modem control pin states (RTS and DTR)
  message SerialProxySetModemPinsRequest {
      ...
  }
  ```
- **Request-type enum**: `aioesphomeapi/tests/test_model.py`:
  ```py
  assert SerialProxyRequestType.convert(4) == SerialProxyRequestType.SET_MODEM_PINS
  ```
- **ESPHome C++ implementation**: `esphome/components/serial_proxy/serial_proxy.h`:
  ```h
  SerialProxyResult configure(api::APIConnection *api_connection,
                              uint32_t baudrate, bool flow_control,
                              uint8_t parity, uint8_t stop_bits, uint8_t data_size);
  ```
  and `serial_proxy.cpp`:
  ```cpp
  if (this->dtr_pin_ != nullptr) {
      this->dtr_pin_->setup();
      ...
  }
  ```
- **ESPHome docs** (`src/content/docs/components/serial_proxy.mdx`):
  > Control modem pins — if `rts_pin` or `dtr_pin` are configured, the API client can set their states at runtime.

**Consequence for MR4U Phase 1 firmware:** the CC2674 `serial_proxy:` YAML block must specify `dtr_pin:` and `rts_pin:` wired to the ESP32-S3 GPIOs that connect to CC2674 `nRESET` and `BSL_ENTRY` respectively (per `docs/hardware-map.md`). Without this, radio flashing via ZHA/zigpy-znp will fail with an "unsupported operation" style error, and the user will have to fall back to reflashing via ESPHome-native OTA (which requires a physical button press or a wired serial jig — a serious UX regression).

Similarly for the EFR32 side (`serial_proxy:` block for `thread`/`efr32`), the EFR32 bootloader entry is via a boot pin held during reset. If ZHA is ever used with EFR32 as the coordinator (bellows path), the same `dtr_pin:` / `rts_pin:` wiring must be present on that `serial_proxy:` block.

## 6. Fitness for our design

Mapping ZHA-chain capabilities onto design requirements:

| Requirement (from design doc) | ZHA → zigpy → backend | Notes |
|---|:---:|---|
| ZHA opens the Zigbee coordinator through `esphome-hass://` end-to-end | ✓ | Confirmed for `zigpy-znp` (CC2674) and `bellows` (EFR32) |
| No layer calls `pyserial` / `pyserial-asyncio` directly | ✓ (for our radios) | `zigpy-znp` and `bellows` both route through `zigpy.serial` → `serialx`. `zigpy-cc` bypasses but is not one of our radios. |
| ZHA does not need `esphome-hass://`-specific patches | ✓ | ZHA treats `CONF_DEVICE_PATH` as opaque |
| Radio flashing over the encrypted API is possible | ✓ | End-to-end DTR/RTS control wired via `SerialProxySetModemPinsRequest` — provided firmware wires `dtr_pin:` / `rts_pin:` |
| Baud reconfigure during bootloader entry | ✓ | `SerialProxyConfigureRequest` supports baud change (Phase 0.2). CC26xx BSL runs at 460800 by default. |
| Runtime behaviour under reconnect | Partial | Phase 0.3 confirmed startup-order handling. Live-reconnect race still Phase 3. |
| Flow-control mismatch does not break open | Empirical | See §7 open question |

## 7. What Phase 0.4 did NOT verify

1. **Whether serialx silently swallows `rtscts=True` for `esphome-hass://` or forwards it as a CONFIGURE request that then fails with `NOT_SUPPORTED`.** `bellows` will pass `rtscts=True` when `CONF_DEVICE_FLOW_CONTROL="hardware"` (HA's default for EFR32). If serialx forwards it, we need HA's ZHA config to explicitly set `flow_control=None` for EFR32-behind-ESPHome, or we need a small upstream tweak in `serialx.platforms.serial_esphome` to no-op flow-control kwargs. This is a Phase 2 empirical validation — worst case, we file a serialx PR to make the ESPHome transport ignore local flow-control kwargs.
2. **The full zigpy-znp bootloader-entry sequence over Noise-encrypted UART.** The proto messages exist and the wiring is correct; but whether the microsecond-level RTS/DTR toggle timing survives the round-trip through Noise + Wi-Fi + ESP GPIO scheduling latency is empirical. If it doesn't, coordinator flashing has to fall back to a wired jig, which is a Phase 4/7 concern, not a Phase 1 blocker.
3. **`zigpy-cc` behaviour.** Confirmed bypasses `serialx`. Would need a small PR to fix (route through `zigpy.serial.create_serial_connection`). We are ignoring this because we ship CC2674, not CC2531 — but if we ever wanted to support legacy CC2531 backhaul it would need attention.

## 8. Implications for the design doc

Findings to reflect into `docs/design.md`:

| Finding | Affects design doc section | Nature of change |
|---|---|---|
| ZHA → zigpy → zigpy-znp / bellows all route through `zigpy.serial` → `serialx` | §10 (validation), §21.4 | Resolve §21.4 unknown; downgrade §10 validation risk to empirical only |
| `zigpy-cc` bypasses serialx but is irrelevant to our radios | §10, §21.4 | Add scoped note: "our radio families are covered; legacy CC2531 backend is not" |
| ZHA never inspects the URL | §7, §9, §21 | Add: no ZHA-specific plumbing needed |
| DTR/RTS control is wired end-to-end for radio flashing | Phase 1 checklist, §11 (radio flashing) | Add: `dtr_pin:` / `rts_pin:` MUST be configured in the CC2674 `serial_proxy:` YAML block |
| Bellows quirky flow-control default; local pyserial-style kwargs must no-op through ESPHome | §21 (new empirical item) | Add small open question — Phase 2 validation |

## 9. Update to project risk profile

Before Phase 0.4, one of the top structural risks was: "some radio backend calls `pyserial` directly and can't open `esphome-hass://`." **That risk is eliminated** for our radios (CC2674 via zigpy-znp; EFR32 via bellows). What remains for Phase 2 is purely runtime-behavioral validation (bootloader-entry timing, flow-control kwarg handling, live reconnect races). Those are empirical, not structural.

Phase 0 is now substantively complete for the HA / zigpy / ESPHome axis. The only remaining Phase 0 item is OTBR (Phase 0.5) — architecturally independent because OTBR runs in a separate add-on container.
