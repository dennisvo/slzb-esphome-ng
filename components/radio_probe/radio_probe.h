// Radio firmware version probe — dispatcher header.
//
// Runs once at boot at setup_priority 250 (after the UART bus is up at
// ~1000, well before serial_proxy attaches at AFTER_CONNECTION ~-30). See
// docs/v1-radio-firmware.md §6d for the timing rationale.

#pragma once

#include <string>

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

namespace esphome {
namespace radio_probe {

static const char *const TAG = "radio_probe";

// docs/v1-radio-firmware.md §6a: ZNP SYS_VERSION round-trip is <50 ms.
static const uint32_t PROBE_TIMEOUT_MS = 200;

class RadioProbe : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void dump_config() override;
  // 250 = after uart::BUS (~1000), before serial_proxy AFTER_CONNECTION (~-30).
  float get_setup_priority() const override { return 250.0f; }

  void set_chip(const std::string &s) { this->chip_ = s; }
  void set_smlight_id(const std::string &s) { this->smlight_id_ = s; }
  void set_protocol(const std::string &s) { this->protocol_ = s; }
  void set_role(const std::string &s) { this->role_ = s; }
  void set_firmware_channel(const std::string &s) { this->firmware_channel_ = s; }
  void set_uart_baud(const std::string &s) { this->uart_baud_ = s; }
  void set_installed_firmware_sensor(text_sensor::TextSensor *s) { this->installed_ = s; }

 protected:
  void dispatch_();
  void publish_(const std::string &value);
  void drain_rx_();

  // Per-protocol probes. Return true on success; on true, `result` holds the
  // human-readable version string to publish.
  bool probe_znp_(std::string &result);
  bool probe_spinel_(std::string &result);

  std::string chip_;
  std::string smlight_id_;
  std::string protocol_;
  std::string role_;
  std::string firmware_channel_;
  std::string uart_baud_;
  text_sensor::TextSensor *installed_{nullptr};
  // Temporary diagnostic: flipped true from setup(); read from dump_config().
  bool setup_ran_{false};
};

}  // namespace radio_probe
}  // namespace esphome
