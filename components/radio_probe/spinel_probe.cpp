// Spinel PROP_VALUE_GET(NCP_VERSION) probe — EFR32MG26 / EFR32MG24
// OpenThread RCP (also NCP). Wire format is HDLC-lite (RFC 1662 byte
// stuffing, no windowing/ACK) around a Spinel payload.
//
// Frame layout (bytes on the wire):
//
//   0x7E ⟨escaped payload⟩ ⟨escaped CRC-LSB⟩ ⟨escaped CRC-MSB⟩ 0x7E
//
// Payload (unescaped, CRC excluded):
//
//   ┌─────────┬──────────┬──────────┐
//   │ header  │ command  │ prop_id  │
//   │ 0x81    │ 0x02     │ 0x02     │      ← request  (PROP_VALUE_GET, NCP_VERSION)
//   └─────────┴──────────┴──────────┘
//
//   ┌─────────┬──────────┬──────────┬──────────────────────────┐
//   │ header  │ command  │ prop_id  │ UTF-8 string (0x00 term) │
//   │ 0x81    │ 0x06     │ 0x02     │ "OPENTHREAD/…"           │  ← response (PROP_VALUE_IS)
//   └─────────┴──────────┴──────────┴──────────────────────────┘
//
//   header byte: bits 7-6 = FLG (0b10), 5-4 = IID (0), 3-0 = TID (we use 1).
//   command / prop_id are Spinel "packed uints"; for values < 128 they are
//     a single byte with high bit clear, which is all we need here.
//   CRC is CCITT-FALSE over the unescaped payload, transmitted LSB first.
//
// The parsed UTF-8 string is published verbatim to the diagnostic sensor;
// HA-side templates handle any normalisation against the SMLIGHT catalog's
// `rev` field (see docs/v1-radio-firmware.md §§2, 6c).
//
// References:
//   OpenThread spinel spec  — src/lib/spinel/spinel.h (Apache-2.0)
//   RFC 1662                — HDLC byte-stuffing / CRC placement
//   docs/v1-radio-firmware.md §6c

#include "protocol_helpers.h"
#include "radio_probe.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace radio_probe {

namespace {

constexpr uint8_t SPINEL_HEADER_FLG_IID0_TID1 = 0x81;
constexpr uint8_t SPINEL_CMD_PROP_VALUE_GET = 0x02;
constexpr uint8_t SPINEL_CMD_PROP_VALUE_IS = 0x06;
constexpr uint8_t SPINEL_PROP_NCP_VERSION = 0x02;

// Large enough for OpenThread NCP_VERSION strings (~100 chars) plus header
// bytes, CRC, worst-case escape expansion (2×), and the two flag delimiters.
constexpr size_t SPINEL_RX_BUF = 512;
constexpr size_t SPINEL_TX_BUF = 32;

}  // namespace

bool RadioProbe::probe_spinel_(std::string &result) {
  this->drain_rx_();

  // ── Build request payload + CRC ─────────────────────────────────────────
  const uint8_t payload[] = {
      SPINEL_HEADER_FLG_IID0_TID1,
      SPINEL_CMD_PROP_VALUE_GET,
      SPINEL_PROP_NCP_VERSION,
  };
  const uint16_t crc = ccitt16_crc(payload, sizeof(payload));
  uint8_t framed[SPINEL_TX_BUF];
  framed[0] = HDLC_FLAG;
  size_t body_len = hdlc_escape(payload, sizeof(payload), &framed[1], sizeof(framed) - 3);
  if (body_len == 0) {
    ESP_LOGW(TAG, "spinel: request escape overflowed TX buffer");
    return false;
  }
  const uint8_t crc_bytes[2] = {static_cast<uint8_t>(crc & 0xFF),
                                static_cast<uint8_t>((crc >> 8) & 0xFF)};
  size_t crc_len = hdlc_escape(crc_bytes, sizeof(crc_bytes), &framed[1 + body_len],
                               sizeof(framed) - 2 - body_len);
  if (crc_len == 0) {
    ESP_LOGW(TAG, "spinel: request CRC escape overflowed TX buffer");
    return false;
  }
  framed[1 + body_len + crc_len] = HDLC_FLAG;
  const size_t frame_len = 1 + body_len + crc_len + 1;

  this->write_array(framed, frame_len);
  this->flush();

  // ── Read one framed response ────────────────────────────────────────────
  uint8_t rx[SPINEL_RX_BUF];
  size_t pos = 0;
  bool in_frame = false;
  const uint32_t deadline = millis() + PROBE_TIMEOUT_MS;

  while (millis() < deadline) {
    while (this->available() > 0) {
      uint8_t b;
      if (!this->read_byte(&b)) {
        break;
      }
      if (b == HDLC_FLAG) {
        if (!in_frame) {
          in_frame = true;  // opening flag — start collecting
          pos = 0;
          continue;
        }
        if (pos == 0) {
          continue;  // adjacent flags (idle) — treat this one as new opener
        }
        // Closing flag — we have a full frame in rx[0..pos).
        goto have_frame;
      }
      if (!in_frame) {
        continue;  // pre-flag garbage
      }
      if (pos >= sizeof(rx)) {
        ESP_LOGW(TAG, "spinel: RX buffer overflow");
        return false;
      }
      rx[pos++] = b;
    }
    delay(1);
  }
  ESP_LOGW(TAG, "spinel: probe timeout after %u ms (%u bytes, in_frame=%d)",
           static_cast<unsigned>(PROBE_TIMEOUT_MS), static_cast<unsigned>(pos),
           static_cast<int>(in_frame));
  return false;

have_frame:
  // ── Unescape + verify CRC ───────────────────────────────────────────────
  uint8_t unesc[SPINEL_RX_BUF];
  size_t unesc_len = hdlc_unescape(rx, pos, unesc, sizeof(unesc));
  if (unesc_len < 5) {
    // Need at least: header + cmd + prop + 2-byte CRC.
    ESP_LOGW(TAG, "spinel: frame too short (%u bytes after unescape)",
             static_cast<unsigned>(unesc_len));
    return false;
  }
  const size_t body_bytes = unesc_len - 2;
  const uint16_t got_crc = static_cast<uint16_t>(unesc[body_bytes]) |
                           (static_cast<uint16_t>(unesc[body_bytes + 1]) << 8);
  const uint16_t want_crc = ccitt16_crc(unesc, body_bytes);
  if (got_crc != want_crc) {
    ESP_LOGW(TAG, "spinel: CRC mismatch got=0x%04X want=0x%04X", got_crc, want_crc);
    return false;
  }

  // ── Validate header, cmd, prop_id ────────────────────────────────────────
  if (unesc[0] != SPINEL_HEADER_FLG_IID0_TID1) {
    ESP_LOGW(TAG, "spinel: unexpected header 0x%02X", unesc[0]);
    return false;
  }
  if (unesc[1] != SPINEL_CMD_PROP_VALUE_IS) {
    ESP_LOGW(TAG, "spinel: expected PROP_VALUE_IS(0x06), got 0x%02X", unesc[1]);
    return false;
  }
  if (unesc[2] != SPINEL_PROP_NCP_VERSION) {
    ESP_LOGW(TAG, "spinel: expected prop NCP_VERSION(0x02), got 0x%02X", unesc[2]);
    return false;
  }

  // ── Extract UTF-8 version string (null-terminated per spec) ─────────────
  const uint8_t *str_start = &unesc[3];
  const size_t str_max = body_bytes - 3;
  size_t str_len = 0;
  while (str_len < str_max && str_start[str_len] != 0x00) {
    str_len++;
  }
  if (str_len == 0) {
    ESP_LOGW(TAG, "spinel: NCP_VERSION string is empty");
    return false;
  }
  result.assign(reinterpret_cast<const char *>(str_start), str_len);
  ESP_LOGD(TAG, "spinel NCP_VERSION: %s", result.c_str());
  return true;
}

}  // namespace radio_probe
}  // namespace esphome
