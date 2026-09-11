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
using esphome::radio_probe::hdlc_escape;
using esphome::radio_probe::hdlc_unescape;
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

int main() {
  test_crc_known_vectors();
  test_hdlc_roundtrip_plain_payload();
  test_hdlc_roundtrip_all_reserved();
  test_hdlc_output_cap_too_small();
  test_hdlc_dangling_escape_rejected();
  std::printf("OK — %d assertions passed\n", g_pass);
  return 0;
}
