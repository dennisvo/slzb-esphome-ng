// Pure byte-level primitives shared between EZSP/ASH and Spinel/HDLC-lite
// probes. No ESPHome dependency — must compile on the host toolchain as-is
// so test/test_protocol_helpers.cpp can link against it with plain `g++`.
//
// See:
//   docs/design/radio-probe-reference.md §6c (protocol summary)
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
// Used by Silicon Labs ASH (EZSP transport) per UG101.
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

// CRC-16/X-25 (poly 0x1021 reflected = 0x8408, init 0xFFFF, reflected input
// and output, XOR-out 0xFFFF). Canonical cross-reference vector:
// "123456789" -> 0x906E. Used by OpenThread's HDLC-Lite (Spinel transport)
// — RFC 1662 PPP FCS-16 lineage. NOT interchangeable with ccitt16_crc even
// though both share poly 0x1021: reflection + XOR-out produce different
// residues for the same input.
inline uint16_t crc16_x25(const uint8_t *buf, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++) {
      crc = (crc & 1) ? static_cast<uint16_t>((crc >> 1) ^ 0x8408) : static_cast<uint16_t>(crc >> 1);
    }
  }
  return static_cast<uint16_t>(~crc);
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

// Extract a YYYYMMDD build-date from an OpenThread NCP_VERSION string.
// OpenThread's Instance::GetVersionString() format has been stable since
// ~2018:  "<vendor>-OPENTHREAD/<sw>_GitHub-<sha>; <platform>; Mmm DD YYYY HH:MM:SS"
// where the tail derives from __DATE__ / __TIME__ macros at compile time.
// The SMLIGHT catalog's `rev` field (chip ids 2/13/21) uses YYYYMMDD, so
// converting lets HA compare wire-observed installed firmware to catalog
// entries by exact string equality.
//
// Returns true and writes 8-char YYYYMMDD into `out` on success. Returns
// false (without modifying `out`) if the trailing "; Mmm DD YYYY" segment
// is missing or malformed — the caller then publishes an "unknown" sentinel
// while the raw string remains available on a separate sensor for triage.
inline bool parse_openthread_build_date(const char *ncp_version, size_t len,
                                        char out[9]) {
  if (ncp_version == nullptr || len < 11) return false;

  // Locate the last "; " separator; the platform + date live after it.
  const char *tail = nullptr;
  for (size_t i = 0; i + 1 < len; i++) {
    if (ncp_version[i] == ';' && ncp_version[i + 1] == ' ') {
      tail = ncp_version + i + 2;
    }
  }
  if (tail == nullptr) return false;
  const size_t tail_len = len - static_cast<size_t>(tail - ncp_version);
  if (tail_len < 11) return false;

  // __DATE__ layout is "Mmm DD YYYY" — an 11-char fixed-width field with a
  // single space between each of the three components. DD may have a leading
  // space rather than a leading zero for days 1-9 ("Apr  6 2026").
  const char *mmm = tail;
  const char *dd = tail + 4;
  const char *yyyy = tail + 7;
  if (mmm[3] != ' ' || tail[6] != ' ') return false;

  static const char *const MONTHS[12] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  int month = 0;
  for (int m = 0; m < 12; m++) {
    if (mmm[0] == MONTHS[m][0] && mmm[1] == MONTHS[m][1] && mmm[2] == MONTHS[m][2]) {
      month = m + 1;
      break;
    }
  }
  if (month == 0) return false;

  // Day: leading space or leading zero acceptable; both digits when >= 10.
  int day = 0;
  const char d0 = dd[0];
  const char d1 = dd[1];
  if (d0 == ' ') {
    if (d1 < '0' || d1 > '9') return false;
    day = d1 - '0';
  } else {
    if (d0 < '0' || d0 > '9' || d1 < '0' || d1 > '9') return false;
    day = (d0 - '0') * 10 + (d1 - '0');
  }
  if (day < 1 || day > 31) return false;

  int year = 0;
  for (int i = 0; i < 4; i++) {
    const char c = yyyy[i];
    if (c < '0' || c > '9') return false;
    year = year * 10 + (c - '0');
  }
  if (year < 2000 || year > 2099) return false;  // catalog convention

  out[0] = static_cast<char>('0' + (year / 1000));
  out[1] = static_cast<char>('0' + ((year / 100) % 10));
  out[2] = static_cast<char>('0' + ((year / 10) % 10));
  out[3] = static_cast<char>('0' + (year % 10));
  out[4] = static_cast<char>('0' + (month / 10));
  out[5] = static_cast<char>('0' + (month % 10));
  out[6] = static_cast<char>('0' + (day / 10));
  out[7] = static_cast<char>('0' + (day % 10));
  out[8] = '\0';
  return true;
}

// Extract the chip-family token from an OpenThread NCP_VERSION string.
//
// Format:  "<vendor>-OPENTHREAD/<sw>_GitHub-<sha>; <PLATFORM>; Mmm DD YYYY ..."
// Silabs SDK stamps <PLATFORM> as "EFR32" (their public GSDK examples) or
// "EFR32MG26" / "EFR32MG24" (private catalog builds). We lowercase-normalise
// the token and map it to the chip enum used by radio-probe-reference.md §6i.
//
// Returns one of: "efr32mg26", "efr32mg24", "efr32" (family only, chip
// variant unresolved), "" (parse failed — caller treats as unknown).
inline const char *parse_openthread_platform(const char *ncp_version, size_t len) {
  if (ncp_version == nullptr || len < 5) return "";

  // Find the FIRST "; " (end of vendor/OPENTHREAD prefix) and the SECOND
  // "; " (end of platform token). Everything between them is the platform.
  const char *first = nullptr;
  const char *second = nullptr;
  for (size_t i = 0; i + 1 < len; i++) {
    if (ncp_version[i] == ';' && ncp_version[i + 1] == ' ') {
      if (first == nullptr) {
        first = ncp_version + i;
      } else {
        second = ncp_version + i;
        break;
      }
    }
  }
  if (first == nullptr || second == nullptr) return "";

  const char *plat = first + 2;
  const size_t plat_len = static_cast<size_t>(second - plat);
  if (plat_len == 0 || plat_len > 32) return "";

  // Case-insensitive prefix/exact match against known Silabs stamps.
  auto ieq = [](const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
      char ca = a[i], cb = b[i];
      if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
      if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
      if (ca != cb) return false;
    }
    return true;
  };

  if (plat_len >= 9 && ieq(plat, "efr32mg26", 9)) return "efr32mg26";
  if (plat_len >= 9 && ieq(plat, "efr32mg24", 9)) return "efr32mg24";
  if (plat_len == 5 && ieq(plat, "efr32", 5)) return "efr32";
  return "";
}

}  // namespace radio_probe
}  // namespace esphome
