// Pure byte-level primitives shared between EZSP/ASH and Spinel/HDLC-lite
// probes. No ESPHome dependency — must compile on the host toolchain as-is
// so test/test_protocol_helpers.cpp can link against it with plain `g++`.
//
// See:
//   docs/v1-radio-firmware.md §6c (protocol summary)
//   RFC 1662 (HDLC framing)
//   Silicon Labs UG101 (ASH — inherits HDLC byte-stuffing + CRC-CCITT)
//   OpenThread spinel spec (HDLC-lite: same framing, no windowing/ACK)

#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace radio_probe {

// HDLC / ASH special bytes. Both protocols reserve the same set.
constexpr uint8_t HDLC_FLAG = 0x7E;      // frame delimiter
constexpr uint8_t HDLC_ESCAPE = 0x7D;    // next byte XOR 0x20
constexpr uint8_t HDLC_XOR = 0x20;
constexpr uint8_t HDLC_XON = 0x11;       // XON/XOFF only reserved when
constexpr uint8_t HDLC_XOFF = 0x13;      //   flow control is enabled

// CRC-CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no XOR-out).
// Canonical cross-reference vector: "123456789" -> 0x29B1.
inline uint16_t ccitt16_crc(const uint8_t *buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= static_cast<uint16_t>(buf[i]) << 8;
    for (int b = 0; b < 8; b++) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

// Escape `in` into `out`. Reserved bytes (FLAG, ESCAPE, XON, XOFF) become
// 2-byte sequences: ESCAPE + (byte XOR 0x20). Returns bytes written to
// `out`, or 0 if `cap` is insufficient (worst case: 2*in_len).
inline size_t hdlc_escape(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < in_len; i++) {
    const uint8_t b = in[i];
    const bool reserved = (b == HDLC_FLAG || b == HDLC_ESCAPE || b == HDLC_XON || b == HDLC_XOFF);
    if (reserved) {
      if (o + 2 > cap) return 0;
      out[o++] = HDLC_ESCAPE;
      out[o++] = static_cast<uint8_t>(b ^ HDLC_XOR);
    } else {
      if (o + 1 > cap) return 0;
      out[o++] = b;
    }
  }
  return o;
}

// Reverse of hdlc_escape. Malformed input (dangling ESCAPE at end) returns 0.
inline size_t hdlc_unescape(const uint8_t *in, size_t in_len, uint8_t *out, size_t cap) {
  size_t o = 0;
  for (size_t i = 0; i < in_len; i++) {
    const uint8_t b = in[i];
    if (b == HDLC_ESCAPE) {
      if (i + 1 >= in_len) return 0;
      if (o + 1 > cap) return 0;
      out[o++] = static_cast<uint8_t>(in[++i] ^ HDLC_XOR);
    } else {
      if (o + 1 > cap) return 0;
      out[o++] = b;
    }
  }
  return o;
}

}  // namespace radio_probe
}  // namespace esphome
