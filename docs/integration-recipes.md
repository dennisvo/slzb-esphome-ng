# Integration recipes

How the pieces wire together: `serialx` transport URL schemes, the ZHA path, and the OTBR add-on's internal PTY adapter. Extracted from `design.md` §§6, 9, 11 so the concrete integration how-tos live in a public, git-tracked doc rather than the private full design document.

For the architectural rationale (why encrypted API only, threat model, security invariants), see [design.md](design.md).

---

## 1. `serialx` URL schemes — the architectural insight

Home Assistant's async serial abstraction `serialx` already supports ESPHome serial proxies natively. It ships with two URL schemes:

| Scheme                                                     | Where it works                                     | Uses                                     |
|------------------------------------------------------------|----------------------------------------------------|------------------------------------------|
| `esphome-hass://<config_entry_id>?port_name=…`             | Inside HA Core (via the connected ESPHome integration) | ZHA and any in-Core integration          |
| `esphome://<host>:6053/?port_name=…` + Noise PSK           | Standalone process (separate container)            | OTBR add-on, external tools              |

Both variants speak the ESPHome Native API with Noise + PSK; both provide full serial semantics (baudrate, RTS/DTR modem-pin control, buffer flush). The current OTBR add-on already contains `serialx` and `zigpy.serial` in its Python runtime.

**Consequences for this design:**

1. **The general-purpose `esphome-radio-proxy` daemon is eliminated.** It is no longer needed as a separate service on the host.
2. **ZHA works directly** via the `esphome-hass://` transport with no PTY and no external daemon — subject to end-to-end validation (see design.md §10).
3. **OTBR still needs a small adapter**, because `otbr-agent` requires a POSIX serial descriptor (radio URL `spinel+hdlc+uart://${device}`). But that adapter lives **inside the OTBR add-on container**, not on the network. See §3 below.

---

## 2. Zigbee integration (ZHA)

**ZHA is the primary target integration for v1.** Zigbee2MQTT is not part of v1 but is not architecturally excluded either — see §2.2. ZHA lives inside HA Core and can therefore use `serialx` with the `esphome-hass://` transport that the ESPHome integration registers automatically for each discovered `serial_proxy`.

**Wire-protocol context.** Zigbee coordinators, like Thread RCPs, are host↔NCP split-stack designs. The Zigbee application/network stack runs on the host (zigpy inside ZHA); the CC2674P10 runs a *Zigbee Network Processor* firmware that exposes a binary command protocol over UART. TI calls this protocol **ZNP**; Silicon Labs' equivalent for EmberZNet chips is **EZSP**. Either way, what our `serial_proxy` tunnels is a stream of NCP-protocol bytes — the coordinator's native language. See §3 for the Thread equivalent (Spinel).

Ideal path:

```text
ZHA -> zigpy -> radio-specific zigpy backend -> serialx -> esphome-hass:// -> ESPHome Native API -> CC2674
```

There is no PTY, no localhost TCP, no extra process.

### 2.1 Pairing devices to the Zigbee network (where is the UI?)

**Short answer: there is no pairing UI in our firmware, and there deliberately never will be. Pairing is 100% a ZHA concern in Home Assistant.**

This is the direct consequence of the storage split documented in the tiered state model (radio NVRAM / ZHA `zigbee.db` / ESP32 = nothing). The ESP32-S3 is a transparent encrypted UART pipe; it has no Zigbee stack, no device database, and no notion of "a device is joining." All of the following live above our firmware:

- The Zigbee stack that actually opens the network for joining (permit-join beacon, association response, TCLK exchange) — runs on the **CC2674P10**, driven by ZHA over the NCP protocol.
- The user-facing "Add device" wizard, QR-code / install-code entry, interview progress, naming, area assignment — **ZHA in Home Assistant**.
- The persistent record of "this IEEE is now on my network" — **`zigbee.db`** (and the coordinator NIB on the CC2674).

#### End-user pairing flow

From an operator's point of view, once the MR4U firmware is flashed and ZHA is pointed at `esphome-hass://<config_entry_id>?port_name=cc2674` (design.md §29.1), pairing is exactly stock ZHA:

1. Home Assistant → **Settings → Devices & services → Zigbee Home Automation → Add device**.
2. ZHA calls `permit_joining(duration)` on zigpy.
3. zigpy → CC2674 zigpy backend → `serialx` → `esphome-hass://` → ESPHome Native API → `serial_proxy` → CC2674 UART → CC2674 broadcasts the permit-join beacon over the air.
4. New device joins, CC2674 completes the association + TCLK exchange, sends the join indication back up the same pipe.
5. ZHA runs its interview, writes the new device into `zigbee.db`, exposes entities in HA.

Every one of those bytes traverses the encrypted Noise session (design.md §7). There is no unencrypted pairing path.

#### What our firmware exposes and does not expose

| Concern | Where it lives | Why not on the ESP32 |
|---|---|---|
| "Open network for N seconds" button | ZHA UI | Requires stack awareness (permit-join duration, TCLK policy). |
| Install-code / QR-code entry | ZHA UI | Cryptographic material for the joining device; belongs with the stack that will use it. |
| Device interview, naming, area | ZHA UI + `zigbee.db` | Pure application state. |
| Channel change, network form / migrate | ZHA UI (uses NCP commands) | Zigbee-stack operations. |
| **Radio-touching maintenance** (reset, bootloader, IEEE flash, energy scan) | ESPHome Native API actions (design.md §15) | These are *hardware* operations on the NCP, not network operations. They are the only pairing-adjacent things we expose, and they're gated by the ownership rule (ZHA must be stopped or force-released first). |

Corollary: **we do not add a "permit join" button, switch, or service to the ESPHome firmware.** Doing so would either duplicate ZHA (and desync from it) or bypass ZHA's device database (leaving a device the NCP knows about but ZHA doesn't). Both are anti-features.

#### Contrast with SLZB-OS

SLZB-OS's web UI exposes buttons like "Zigbee network scan" and adapter-level actions, but it likewise does **not** run a Zigbee stack and does **not** pair devices — pairing was always ZHA's job there too. The difference is only that our management surface is authenticated ESPHome actions instead of cleartext HTTP.

### 2.2 Zigbee2MQTT as a future client (not in v1, not blocked)

Z2M is out of v1 scope, but nothing in this design prevents adding it later. Two paths exist; both are *client-side* work and require no firmware changes:

**Path A — Z2M consumes the encrypted `serial_proxy` directly.**

Z2M's `zigbee-herdsman` layer already accepts remote serial adapters via `tcp://` URLs today. What's missing is an `esphome://` transport for the Node.js side (equivalent to Python's `serialx`). Adding it is a small Node module wrapping an ESPHome Native API client library and presenting a stream interface. Once merged upstream, Z2M configuration becomes:

```yaml
# Zigbee2MQTT configuration.yaml
serial:
  adapter: zstack           # or ezsp, per radio
  port: "esphome://mr4u:6053/?port_name=cc2674&psk=..."
```

Z2M then connects over the same encrypted pipe ZHA/OTBR use. No new firmware port. No LAN exposure. Full parity with ZHA's security posture.

**Path B — dual-radio, dual-stack on one MR4U.**

The MR4U has two independent radios. With the port-name-by-chip convention, operators can point ZHA at one chip and Z2M at the other, running two independent Zigbee networks off the same box, both encrypted. This falls out of the design for free; no additional work required beyond Path A's Node transport.

**What Z2M support must NOT become.**

- No MQTT broker on the ESP32. That reopens exactly the surface design.md §3 closes.
- No Z2M-equivalent baked into ESPHome firmware. Z2M is a host-side application; it runs in a container next to HA and consumes our encrypted API like any other client.
- The ownership rule still applies: **one client per physical radio at a time.** ZHA and Z2M cannot share the same CC2674. Migration between them is "stop one, start the other," not "run both concurrently."

**Effort estimate.** Zero firmware work; one small `zigbee-herdsman` transport module PR. Comparable to the OTBR adapter effort. Would slot naturally into a post-v1 Phase 10 if there's user demand.

---

## 3. Thread integration (OTBR)

**About Spinel.** Spinel is the wire protocol between an OpenThread host and an OpenThread RCP — the Thread equivalent of Zigbee's ZNP/EZSP (see §2). It is defined by the OpenThread project; frame-oriented, binary, carried over UART with HDLC-lite framing (hence the URL scheme `spinel+hdlc+uart://`). Messages are property-get / property-set / command / notification over numbered properties (`PROP_STREAM_NET`, `PROP_MAC_SCAN_STATE`, `PROP_NET_NETWORK_KEY`, etc.) — roughly "USB HID for a Thread radio." Purpose: the RCP handles PHY, 802.15.4 MAC, and tight timing; everything above (MLE, DTLS-based commissioning, IPv6, SRP, mDNS) runs in `otbr-agent` on the host. What our `serial_proxy` tunnels for the Thread radio is Spinel-over-HDLC — the RCP's native language.

`otbr-agent` requires a POSIX serial device (`spinel+hdlc+uart://${device}`). It does not understand `esphome://` or `serialx`. Currently the HA OTBR add-on's network-RCP mode uses `socat` to bridge a TCP RCP into a PTY at `/tmp/ttyOTBR`. We replace that with a small, add-on-internal **ESPHome→PTY adapter**.

### 3.1 Adapter architecture (inside the OTBR add-on)

```text
                    OTBR add-on container
              ┌──────────────────────────────┐
              │  serialx ESPHome transport   │
              │       │                      │
              │   esphome://mr4u:6053/       │
              │        ?port_name=efr32      │
              │       │  (Noise + PSK)       │
              │       ▼                      │
              │   PTY  /tmp/ttyOTBR          │
              │       │                      │
              │       ▼                      │
              │     otbr-agent               │
              └──────────────────────────────┘
```

Characteristics:
- Runs entirely inside the OTBR container. No new network listener. No `127.0.0.1:6638`.
- Uses `serialx[esphome]` + `aioesphomeapi` — both are already essentially present in the add-on's Python stack.
- Speaks only Noise-encrypted ESPHome Native API on the wire.

### 3.2 Add-on configuration (proposed)

Rather than exposing raw internals, use one high-level radio abstraction:

```yaml
radio:
  type: esphome
  host: mr4u.local
  port_name: efr32          # whichever chip is configured for Thread
  psk: !secret mr4u_api_key # same PSK as the ESPHome integration in HA Core
  baudrate: 460800
  flow_control: false
```

Add-on startup logic:

1. Open `esphome://<host>:6053/?port_name=<port_name>` via `serialx` using the PSK.
2. Create PTY `/tmp/ttyOTBR`.
3. Forward the async serial transport ⇄ PTY bidirectionally.
4. Launch `otbr-agent` with `spinel+hdlc+uart:///tmp/ttyOTBR?uart-baudrate=…`.

### 3.3 Supervised failure handling

The HA docs warn that a failed remote-RCP connection can leave stale Thread routes for up to ~30 minutes. The adapter MUST supervise this:

- On ESPHome API disconnect → terminate the PTY and stop `otbr-agent` cleanly (don't feign a connected radio).
- On reconnect → restart `otbr-agent` fresh so it re-establishes Thread state deterministically.

### 3.4 Why not extend OpenThread itself?

Adding `spinel+hdlc+esphome://…` to OpenThread would eliminate the PTY, but it is a large upstream change with a long tail. For v1, the internal PTY adapter is much smaller and keeps OpenThread untouched. Terminology: this is an **application-internal serial adapter**, not a proxy — it exposes no network endpoint.

### 3.5 Commissioning devices to the Thread network (where is the UI?)

Same principle as §2.1: **our firmware does not commission Thread devices and does not host a commissioning UI.** The EFR32MG26 runs an RCP (radio co-processor) — the OpenThread stack lives in `otbr-agent` inside the OTBR add-on, and the commissioning UX lives in Home Assistant's **Thread** integration (plus, for Matter devices, the **Matter** integration).

End-user flow, once the OTBR add-on is pointed at our `esphome://<host>:6053/?port_name=efr32`:

1. HA → **Settings → Devices & services → Thread** (dataset management, credential sharing with Apple/Google borders) — or the **Matter** integration for adding a Matter-over-Thread device via QR code / setup code.
2. HA drives commissioning via `otbr-agent`, which drives the EFR32 RCP over Spinel, over our encrypted UART pipe.
3. Persistent Thread state (network key, PAN ID, channel, active operational dataset, child table) lives on the **EFR32 flash** and in the **OTBR add-on's data volume** — not on the ESP32.

We expose no "join Thread device" action in ESPHome. The only Thread-related things we expose are the same class of hardware maintenance as Zigbee (`thread_radio_reset`, `thread_radio_bootloader`), gated by the ownership rule (OTBR must be stopped or force-released first, because they contend for the same UART).
