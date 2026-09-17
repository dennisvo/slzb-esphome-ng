// ZNP SYS_VERSION probe — CC2674P10 / CC1352P7 Zigbee coordinator + router.
//
// Wire format (Texas Instruments MT/UNPI, "General" transport, 1-byte length):
//
//   Request  frame: [SOF=0xFE][LEN=0x00][CMD0=0x21][CMD1=0x02][FCS]
//                   CMD0 0x21 = SYS SREQ, CMD1 0x02 = SYS_VERSION
//   Response frame: [SOF=0xFE][LEN=0x09][CMD0=0x61][CMD1=0x02][data*9][FCS]
//                   data = TransportRev(1) Product(1) MajorRel(1) MinorRel(1)
//                          MaintRel(1) Revision(u32 LE)
//   FCS = XOR of LEN, CMD0, CMD1, all data bytes.
//
// The Revision u32 is a YYYYMMDD build-date integer (e.g. 20260311). We
// emit it as a decimal string — that's exactly what the SMLIGHT catalog's
// `rev` field uses (see docs/design/radio-probe-reference.md §2 schema).
//
// Timing: docs/design/radio-probe-reference.md §6a says round-trip is <50 ms in
// practice; we cap at PROBE_TIMEOUT_MS (200 ms) to bound boot delay.
//
// References:
//   docs/design/radio-probe-reference.md §6a  — protocol description
//   zigpy-znp / Z-Stack MT_SYS spec — canonical wire format

#include "radio_probe.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace radio_probe {

namespace {

constexpr uint8_t ZNP_SOF = 0xFE;
constexpr uint8_t ZNP_LEN_REQ = 0x00;
constexpr uint8_t ZNP_CMD0_SREQ_SYS = 0x21;
constexpr uint8_t ZNP_CMD0_SRSP_SYS = 0x61;
constexpr uint8_t ZNP_CMD1_VERSION = 0x02;
constexpr size_t ZNP_MIN_PAYLOAD = 9;  // TransportRev+Product+3xRel+u32 Revision
constexpr size_t ZNP_MAX_FRAME = 32;   // SOF+LEN+CMD0+CMD1+payload+FCS well under 32

// MT_UTIL subsystem (0x07) — UTIL_GET_DEVICE_INFO returns a fixed-layout
// payload starting with Status(1)+IEEEAddr(8)+ShortAddr(2)+DeviceType(1)+…
// See TI Z-Stack Monitor & Test API §UTIL_GET_DEVICE_INFO.
constexpr uint8_t ZNP_CMD0_SREQ_UTIL = 0x27;
constexpr uint8_t ZNP_CMD0_SRSP_UTIL = 0x67;
constexpr uint8_t ZNP_CMD1_GET_DEVICE_INFO = 0x00;
constexpr size_t ZNP_UTIL_MIN_PAYLOAD = 12;  // status+ieee(8)+short(2)+devtype
constexpr size_t ZNP_UTIL_MAX_FRAME = 64;    // ends with 1-byte AssocDevCnt + up to N*2 shorts

uint8_t xor_fcs(const uint8_t *bytes, size_t len) {
  uint8_t fcs = 0;
  for (size_t i = 0; i < len; i++) {
    fcs ^= bytes[i];
  }
  return fcs;
}

// SYS_VERSION Product byte → chip family label. TI Z-Stack + SMLIGHT builds
// use 0=CC2530, 1=CC2538, 2=CC26x2/CC1352 (all newer chips in one bucket).
// Distinguishing CC2674P10 vs CC1352P7 vs CC1352P2 within the CC26xx family
// requires reading a chip-id NV item that isn't part of standard MT_SYS —
// see docs/design/radio-probe-reference.md §6i (family-only path deferred to v1.x).
const char *znp_product_to_chip_family(uint8_t product) {
  switch (product) {
    case 0: return "cc2530";
    case 1: return "cc2538";
    case 2: return "cc26xx_family";
    default: return "";
  }
}

// UTIL_GET_DEVICE_INFO DeviceType byte → coord/router/end-device. Coord vs
// router is the only distinction we care about for the SMLIGHT catalog
// `type` field (0 vs 1). End-device support is out of scope for the fork
// but reported honestly if the wire says so.
const char *znp_device_type_to_role(uint8_t devtype) {
  switch (devtype) {
    case 0x00: return "coord";
    case 0x01: return "router";
    case 0x02: return "end_device";
    default: return "";
  }
}

}  // namespace

bool RadioProbe::probe_znp_(std::string &rev, std::string &raw, std::string &chip_probed,
                            std::string &role_probed) {
  this->drain_rx_();

  const uint8_t req_body[] = {ZNP_LEN_REQ, ZNP_CMD0_SREQ_SYS, ZNP_CMD1_VERSION};
  const uint8_t req_fcs = xor_fcs(req_body, sizeof(req_body));
  const uint8_t frame[] = {ZNP_SOF, req_body[0], req_body[1], req_body[2], req_fcs};

  this->write_array(frame, sizeof(frame));
  this->flush();

  uint8_t buf[ZNP_MAX_FRAME];
  size_t pos = 0;
  bool sof_seen = false;
  const uint32_t deadline = millis() + PROBE_TIMEOUT_MS;

  while (millis() < deadline) {
    while (this->available() > 0 && pos < sizeof(buf)) {
      uint8_t b;
      if (!this->read_byte(&b)) {
        break;
      }
      if (!sof_seen) {
        if (b == ZNP_SOF) {
          sof_seen = true;
          buf[pos++] = b;
        }
        continue;  // discard pre-SOF garbage
      }
      buf[pos++] = b;
    }

    if (pos >= 5) {
      const size_t payload_len = buf[1];
      const size_t frame_len = 1 /*SOF*/ + 1 /*LEN*/ + 2 /*CMD*/ + payload_len + 1 /*FCS*/;
      if (frame_len > sizeof(buf)) {
        ESP_LOGW(TAG, "ZNP frame length %u exceeds buffer", static_cast<unsigned>(frame_len));
        return false;
      }
      if (pos >= frame_len) {
        // FCS covers LEN + CMD0 + CMD1 + payload.
        const uint8_t got_fcs = buf[frame_len - 1];
        const uint8_t want_fcs = xor_fcs(&buf[1], frame_len - 2);
        if (got_fcs != want_fcs) {
          ESP_LOGW(TAG, "ZNP FCS mismatch: got 0x%02X want 0x%02X", got_fcs, want_fcs);
          return false;
        }
        if (buf[2] != ZNP_CMD0_SRSP_SYS || buf[3] != ZNP_CMD1_VERSION) {
          ESP_LOGW(TAG, "ZNP unexpected response CMD0=0x%02X CMD1=0x%02X", buf[2], buf[3]);
          return false;
        }
        if (payload_len < ZNP_MIN_PAYLOAD) {
          ESP_LOGW(TAG, "ZNP payload too short: %u", static_cast<unsigned>(payload_len));
          return false;
        }
        // Payload layout: [4]=TransportRev [5]=Product [6]=Major [7]=Minor
        //                 [8]=Maint [9..12]=Revision (u32 LE, YYYYMMDD)
        const uint32_t rev_u32 = static_cast<uint32_t>(buf[9]) |
                                 (static_cast<uint32_t>(buf[10]) << 8) |
                                 (static_cast<uint32_t>(buf[11]) << 16) |
                                 (static_cast<uint32_t>(buf[12]) << 24);
        ESP_LOGD(TAG, "ZNP SYS_VERSION: transport=%u product=%u %u.%u.%u rev=%u", buf[4], buf[5],
                 buf[6], buf[7], buf[8], static_cast<unsigned>(rev_u32));
        char out[16];
        snprintf(out, sizeof(out), "%u", static_cast<unsigned>(rev_u32));
        rev = out;
        // Raw descriptor mirrors what dump_config would log: every byte we
        // decoded, laid out as one line so HA can display the full response
        // for triage when catalog matching fails.
        char raw_buf[96];
        snprintf(raw_buf, sizeof(raw_buf),
                 "transport=%u product=%u %u.%u.%u rev=%u", buf[4], buf[5], buf[6], buf[7],
                 buf[8], static_cast<unsigned>(rev_u32));
        raw = raw_buf;
        chip_probed = znp_product_to_chip_family(buf[5]);
        // Role probe is a separate MT/UNPI round-trip; ignore its failure —
        // the SYS_VERSION result is the primary success signal.
        std::string role_out;
        if (this->probe_znp_role_(role_out)) {
          role_probed = role_out;
        }
        return true;
      }
    }
    delay(1);
  }

  ESP_LOGW(TAG, "ZNP probe timeout after %u ms (%u bytes)", static_cast<unsigned>(PROBE_TIMEOUT_MS),
           static_cast<unsigned>(pos));
  return false;
}

bool RadioProbe::probe_znp_role_(std::string &role_probed) {
  this->drain_rx_();

  const uint8_t req_body[] = {ZNP_LEN_REQ, ZNP_CMD0_SREQ_UTIL, ZNP_CMD1_GET_DEVICE_INFO};
  const uint8_t req_fcs = xor_fcs(req_body, sizeof(req_body));
  const uint8_t frame[] = {ZNP_SOF, req_body[0], req_body[1], req_body[2], req_fcs};

  this->write_array(frame, sizeof(frame));
  this->flush();

  uint8_t buf[ZNP_UTIL_MAX_FRAME];
  size_t pos = 0;
  bool sof_seen = false;
  const uint32_t deadline = millis() + PROBE_TIMEOUT_MS;

  while (millis() < deadline) {
    while (this->available() > 0 && pos < sizeof(buf)) {
      uint8_t b;
      if (!this->read_byte(&b)) {
        break;
      }
      if (!sof_seen) {
        if (b == ZNP_SOF) {
          sof_seen = true;
          buf[pos++] = b;
        }
        continue;
      }
      buf[pos++] = b;
    }

    if (pos >= 5) {
      const size_t payload_len = buf[1];
      const size_t frame_len = 1 + 1 + 2 + payload_len + 1;
      if (frame_len > sizeof(buf)) {
        ESP_LOGW(TAG, "ZNP UTIL frame length %u exceeds buffer",
                 static_cast<unsigned>(frame_len));
        return false;
      }
      if (pos >= frame_len) {
        const uint8_t got_fcs = buf[frame_len - 1];
        const uint8_t want_fcs = xor_fcs(&buf[1], frame_len - 2);
        if (got_fcs != want_fcs) {
          ESP_LOGW(TAG, "ZNP UTIL FCS mismatch: got 0x%02X want 0x%02X", got_fcs, want_fcs);
          return false;
        }
        if (buf[2] != ZNP_CMD0_SRSP_UTIL || buf[3] != ZNP_CMD1_GET_DEVICE_INFO) {
          ESP_LOGW(TAG, "ZNP UTIL unexpected response CMD0=0x%02X CMD1=0x%02X", buf[2], buf[3]);
          return false;
        }
        if (payload_len < ZNP_UTIL_MIN_PAYLOAD) {
          ESP_LOGW(TAG, "ZNP UTIL payload too short: %u", static_cast<unsigned>(payload_len));
          return false;
        }
        // Payload: [4]=Status [5..12]=IEEE [13..14]=Short [15]=DeviceType
        const uint8_t devtype = buf[15];
        ESP_LOGD(TAG, "ZNP UTIL_GET_DEVICE_INFO: status=%u devtype=%u", buf[4], devtype);
        const char *role = znp_device_type_to_role(devtype);
        if (role[0] == '\0') {
          ESP_LOGW(TAG, "ZNP UTIL unknown DeviceType 0x%02X", devtype);
          return false;
        }
        role_probed = role;
        return true;
      }
    }
    delay(1);
  }

  ESP_LOGW(TAG, "ZNP UTIL role probe timeout after %u ms (%u bytes)",
           static_cast<unsigned>(PROBE_TIMEOUT_MS), static_cast<unsigned>(pos));
  return false;
}

}  // namespace radio_probe
}  // namespace esphome
