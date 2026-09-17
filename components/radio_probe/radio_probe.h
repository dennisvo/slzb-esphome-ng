// Radio firmware version probe — dispatcher header.
//
// Runs once at boot at setup_priority 250 (after the UART bus is up at
// ~1000, well before serial_proxy attaches at AFTER_CONNECTION ~-30). See
// docs/design/radio-probe-reference.md §6d for the timing rationale.

#pragma once

#include <string>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

namespace esphome {
namespace radio_probe {

static const char *const TAG = "radio_probe";

// docs/design/radio-probe-reference.md §6a: ZNP SYS_VERSION round-trip is <50 ms.
static const uint32_t PROBE_TIMEOUT_MS = 200;

class RadioProbe : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void dump_config() override;
  // 250 = after uart::BUS (~1000), before serial_proxy AFTER_CONNECTION (~-30).
  float get_setup_priority() const override { return 250.0f; }

  void set_chip(const std::string &s) { this->chip_ = s; }
  void set_protocol(const std::string &s) { this->protocol_ = s; }
  void set_role(const std::string &s) { this->role_ = s; }
  void set_firmware_channel(const std::string &s) { this->firmware_channel_ = s; }
  void set_uart_baud(const std::string &s) { this->uart_baud_ = s; }
  void set_installed_firmware_sensor(text_sensor::TextSensor *s) { this->installed_ = s; }
  void set_installed_firmware_raw_sensor(text_sensor::TextSensor *s) { this->installed_raw_ = s; }
  void set_chip_probed_sensor(text_sensor::TextSensor *s) { this->chip_probed_ = s; }
  void set_role_probed_sensor(text_sensor::TextSensor *s) { this->role_probed_ = s; }

 protected:
  void dispatch_();
  void publish_(const std::string &value);
  void publish_raw_(const std::string &value);
  void publish_probed_(const std::string &chip_probed, const std::string &role_probed,
                       bool probe_ok);
  void drain_rx_();

  // Per-protocol probes. Return true on protocol-level success (frame
  // received + validated). On true, `rev` is the normalized YYYYMMDD tag
  // (or an "unknown (…)" sentinel when the wire format doesn't yield a
  // parseable date) and `raw` is the full firmware descriptor for triage.
  // `chip_probed` / `role_probed` receive wire-derived identity strings
  // (may be empty when the probe can't determine them).
  bool probe_znp_(std::string &rev, std::string &raw, std::string &chip_probed,
                  std::string &role_probed);
  bool probe_spinel_(std::string &rev, std::string &raw, std::string &chip_probed,
                     std::string &role_probed);
  // ZNP UTIL_GET_DEVICE_INFO 0x27/0x00 — returns coord/router/end-device
  // as a separate MT/UNPI round-trip. See docs/design/radio-probe-reference.md §6a.
  bool probe_znp_role_(std::string &role_probed);

  std::string chip_;
  std::string protocol_;
  std::string role_;
  std::string firmware_channel_;
  std::string uart_baud_;
  text_sensor::TextSensor *installed_{nullptr};
  text_sensor::TextSensor *installed_raw_{nullptr};
  text_sensor::TextSensor *chip_probed_{nullptr};
  text_sensor::TextSensor *role_probed_{nullptr};
};

}  // namespace radio_probe
}  // namespace esphome
