// Host-compilable regression tests for components/radio_probe/protocol_helpers.h.
//
// Build & run (from repo root):
//   g++ -std=c++17 -Icomponents/radio_probe components/radio_probe/test/test_protocol_helpers.cpp -o test_helpers.exe && ./test_helpers.exe
//
// Zero runtime cost on device — this file is never compiled by the ESPHome
// build. It exists only as a fast local sanity check for the pure functions
// in protocol_helpers.h, which are shared by spinel_probe.cpp and (later)
// ezsp_probe.cpp.

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../protocol_helpers.h"

using esphome::radio_probe::ccitt16_crc;
using esphome::radio_probe::crc16_x25;
using esphome::radio_probe::hdlc_escape;
using esphome::radio_probe::hdlc_unescape;
using esphome::radio_probe::parse_openthread_build_date;
using esphome::radio_probe::parse_openthread_platform;
using esphome::radio_probe::HDLC_FLAG;
using esphome::radio_probe::HDLC_ESCAPE;
using esphome::radio_probe::HDLC_XON;
using esphome::radio_probe::HDLC_XOFF;

static int g_pass = 0;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
      std::exit(1);                                                    \
    }                                                                  \
    ++g_pass;                                                          \
  } while (0)

static void test_crc_known_vectors() {
  // Empty input keeps the initial value.
  CHECK(ccitt16_crc(nullptr, 0) == 0xFFFF);

  // Canonical CRC-16-CCITT-FALSE cross-reference vector.
  const uint8_t v123456789[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(ccitt16_crc(v123456789, sizeof(v123456789)) == 0x29B1);

  // Single byte, low-value.
  const uint8_t vA[] = {'A'};
  CHECK(ccitt16_crc(vA, sizeof(vA)) == 0xB915);

  // Canonical CRC-16/X-25 cross-reference vector (RFC 1662 FCS-16 residue
  // convention). Guards against silent regressions of the OpenThread
  // HDLC-Lite CRC used by spinel_probe.cpp.
  CHECK(crc16_x25(v123456789, sizeof(v123456789)) == 0x906E);

  // Confirms the two CCITT variants are NOT interchangeable despite sharing
  // the same polynomial — the trap that produced the spinel probe CRC-mismatch
  // bug we shipped in v1.
  CHECK(ccitt16_crc(v123456789, sizeof(v123456789)) !=
        crc16_x25(v123456789, sizeof(v123456789)));
}

static void test_hdlc_roundtrip_plain_payload() {
  const uint8_t in[] = {0x00, 0x01, 0x42, 0xAA, 0xFF};
  uint8_t esc[16];
  uint8_t back[16];
  size_t n = hdlc_escape(in, sizeof(in), esc, sizeof(esc));
  CHECK(n == sizeof(in));  // no reserved bytes -> no expansion
  size_t m = hdlc_unescape(esc, n, back, sizeof(back));
  CHECK(m == sizeof(in));
  CHECK(std::memcmp(in, back, sizeof(in)) == 0);
}

static void test_hdlc_roundtrip_all_reserved() {
  const uint8_t in[] = {HDLC_FLAG, HDLC_ESCAPE, HDLC_XON, HDLC_XOFF, 0x42};
  uint8_t esc[32];
  uint8_t back[16];
  size_t n = hdlc_escape(in, sizeof(in), esc, sizeof(esc));
  // Four reserved bytes -> +4 bytes expansion; one plain byte stays 1.
  CHECK(n == sizeof(in) + 4);
  // Reserved bytes must be replaced with 0x7D + (byte ^ 0x20).
  CHECK(esc[0] == HDLC_ESCAPE);
  CHECK(esc[1] == (HDLC_FLAG ^ 0x20));
  CHECK(esc[2] == HDLC_ESCAPE);
  CHECK(esc[3] == (HDLC_ESCAPE ^ 0x20));
  CHECK(esc[8] == 0x42);  // plain byte survives at end

  size_t m = hdlc_unescape(esc, n, back, sizeof(back));
  CHECK(m == sizeof(in));
  CHECK(std::memcmp(in, back, sizeof(in)) == 0);
}

static void test_hdlc_output_cap_too_small() {
  const uint8_t in[] = {HDLC_FLAG};
  uint8_t esc[1];  // needs 2 bytes -> must refuse
  CHECK(hdlc_escape(in, sizeof(in), esc, sizeof(esc)) == 0);
}

static void test_hdlc_dangling_escape_rejected() {
  const uint8_t bad[] = {HDLC_ESCAPE};  // no follower
  uint8_t out[8];
  CHECK(hdlc_unescape(bad, sizeof(bad), out, sizeof(out)) == 0);
}

static void test_openthread_build_date_canonical() {
  // Live MR4U boot log, EFR32MG26 3.0.1 Thread RCP (rev 20260416).
  const char *v = "SL-OPENTHREAD/3.0.1.0_GitHub-61e43cffb; EFR32; Apr 16 2026 06:14:46";
  char out[9] = {0};
  CHECK(parse_openthread_build_date(v, std::strlen(v), out));
  CHECK(std::strcmp(out, "20260416") == 0);
}

static void test_openthread_build_date_leading_space_day() {
  // __DATE__ pads single-digit days with a leading space, not a zero.
  const char *v = "SL-OPENTHREAD/3.0.1.0_GitHub-abcdefg; EFR32; Apr  6 2026 06:14:46";
  char out[9] = {0};
  CHECK(parse_openthread_build_date(v, std::strlen(v), out));
  CHECK(std::strcmp(out, "20260406") == 0);
}

static void test_openthread_build_date_all_months() {
  // Every month token must resolve to the correct 1..12 index.
  const char *inputs[12] = {
      "vendor; plat; Jan 01 2026 00:00:00", "vendor; plat; Feb 02 2026 00:00:00",
      "vendor; plat; Mar 03 2026 00:00:00", "vendor; plat; Apr 04 2026 00:00:00",
      "vendor; plat; May 05 2026 00:00:00", "vendor; plat; Jun 06 2026 00:00:00",
      "vendor; plat; Jul 07 2026 00:00:00", "vendor; plat; Aug 08 2026 00:00:00",
      "vendor; plat; Sep 09 2026 00:00:00", "vendor; plat; Oct 10 2026 00:00:00",
      "vendor; plat; Nov 11 2026 00:00:00", "vendor; plat; Dec 12 2026 00:00:00",
  };
  const char *expected[12] = {
      "20260101", "20260202", "20260303", "20260404", "20260505", "20260606",
      "20260707", "20260808", "20260909", "20261010", "20261111", "20261212",
  };
  for (int i = 0; i < 12; i++) {
    char out[9] = {0};
    CHECK(parse_openthread_build_date(inputs[i], std::strlen(inputs[i]), out));
    CHECK(std::strcmp(out, expected[i]) == 0);
  }
}

static void test_openthread_build_date_malformed() {
  char out[9] = {0};

  // Empty / too short.
  CHECK(!parse_openthread_build_date("", 0, out));
  CHECK(!parse_openthread_build_date("short", 5, out));

  // No "; " separator anywhere.
  const char *no_sep = "no-separator-anywhere-in-string";
  CHECK(!parse_openthread_build_date(no_sep, std::strlen(no_sep), out));

  // Invalid month token.
  const char *bad_month = "vendor; plat; Xyz 06 2026 00:00:00";
  CHECK(!parse_openthread_build_date(bad_month, std::strlen(bad_month), out));

  // Day out of range.
  const char *bad_day = "vendor; plat; Apr 42 2026 00:00:00";
  CHECK(!parse_openthread_build_date(bad_day, std::strlen(bad_day), out));

  // Year out of catalog window.
  const char *bad_year = "vendor; plat; Apr 16 1999 00:00:00";
  CHECK(!parse_openthread_build_date(bad_year, std::strlen(bad_year), out));

  // Missing space between components.
  const char *bad_space = "vendor; plat; Apr16 2026 00:00:00";
  CHECK(!parse_openthread_build_date(bad_space, std::strlen(bad_space), out));
}

static void test_openthread_platform_canonical() {
  // Live MR4U boot log, EFR32MG26 3.0.1 Thread RCP.
  const char *v = "SL-OPENTHREAD/3.0.1.0_GitHub-61e43cffb; EFR32MG26; Apr 16 2026 06:14:46";
  CHECK(std::strcmp(parse_openthread_platform(v, std::strlen(v)), "efr32mg26") == 0);

  const char *w = "SL-OPENTHREAD/3.0.1.0_GitHub-abc; EFR32MG24; Apr 16 2026 06:14:46";
  CHECK(std::strcmp(parse_openthread_platform(w, std::strlen(w)), "efr32mg24") == 0);
}

static void test_openthread_platform_family_only() {
  // Public Silabs GSDK examples stamp the platform as bare "EFR32".
  const char *v = "SL-OPENTHREAD/3.0.1.0_GitHub-abc; EFR32; Apr 16 2026 06:14:46";
  CHECK(std::strcmp(parse_openthread_platform(v, std::strlen(v)), "efr32") == 0);
}

static void test_openthread_platform_case_insensitive() {
  const char *v = "SL-OPENTHREAD/3.0.1.0_GitHub-abc; efr32mg26; Apr 16 2026 06:14:46";
  CHECK(std::strcmp(parse_openthread_platform(v, std::strlen(v)), "efr32mg26") == 0);
}

static void test_openthread_platform_malformed() {
  // Only one "; " — no closing delimiter for platform token.
  const char *one_sep = "SL-OPENTHREAD/3.0.1; EFR32MG26";
  CHECK(std::strcmp(parse_openthread_platform(one_sep, std::strlen(one_sep)), "") == 0);

  // Unknown platform token → empty result (caller treats as unknown).
  const char *unknown = "SL-OPENTHREAD/3.0.1; CC2652P; Apr 16 2026 06:14:46";
  CHECK(std::strcmp(parse_openthread_platform(unknown, std::strlen(unknown)), "") == 0);

  // No separator at all.
  const char *no_sep = "no-separator-anywhere";
  CHECK(std::strcmp(parse_openthread_platform(no_sep, std::strlen(no_sep)), "") == 0);

  // Empty input.
  CHECK(std::strcmp(parse_openthread_platform("", 0), "") == 0);
}

int main() {
  test_crc_known_vectors();
  test_hdlc_roundtrip_plain_payload();
  test_hdlc_roundtrip_all_reserved();
  test_hdlc_output_cap_too_small();
  test_hdlc_dangling_escape_rejected();
  test_openthread_build_date_canonical();
  test_openthread_build_date_leading_space_day();
  test_openthread_build_date_all_months();
  test_openthread_build_date_malformed();
  test_openthread_platform_canonical();
  test_openthread_platform_family_only();
  test_openthread_platform_case_insensitive();
  test_openthread_platform_malformed();
  std::printf("OK — %d assertions passed\n", g_pass);
  return 0;
}
