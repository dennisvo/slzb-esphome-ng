// Radio firmware version probe — dispatcher.
//
// The v1 dispatcher is a switch on the `protocol` substitution:
//   znp     -> live ZNP SYS_VERSION probe (znp_probe.cpp)
//   spinel  -> live Spinel PROP_VALUE_GET(NCP_VERSION) probe (spinel_probe.cpp)
//   ezsp    -> stub (deferred to v1.x per roadmap.md)
//   zwave   -> stub (deferred to v1.x)
//   none    -> no sensor emitted (radioless board)
//
// See docs/design/radio-probe-reference.md §§5, 6d, 6f.

#include "radio_probe.h"

#include "esphome/core/log.h"

#ifdef USE_ESP_IDF
#include "driver/uart.h"

#include "esphome/components/uart/uart_component_esp_idf.h"
#else
#error "This fork requires the ESP-IDF framework. See packages/core/core.yaml."
#endif

namespace esphome {
namespace radio_probe {

void RadioProbe::setup() {
  // Delay dispatch so the radio has time to boot and (when HW flow control
  // is enabled) assert CTS before we write the first probe frame. Without
  // this, an ESP32 POR that beats the radio to ready leaves flush() waiting
  // on a CTS that never arrives → task WDT → OTA rollback. Probe is
  // best-effort so 300 ms is a comfortable margin.
  this->set_timeout("dispatch", 300, [this]() { this->dispatch_(); });
}

bool RadioProbe::flush_bounded_(uint32_t timeout_ms) {
  if (this->parent_ == nullptr) {
    return false;
  }
  auto *idf = static_cast<uart::IDFUARTComponent *>(this->parent_);
  const uart_port_t port = static_cast<uart_port_t>(idf->get_hw_serial_number());
  return uart_wait_tx_done(port, pdMS_TO_TICKS(timeout_ms)) == ESP_OK;
}

void RadioProbe::dump_config() {
  ESP_LOGCONFIG(TAG, "Radio Probe:");
  ESP_LOGCONFIG(TAG, "  Chip: %s", this->chip_.c_str());
  ESP_LOGCONFIG(TAG, "  Protocol: %s", this->protocol_.c_str());
  ESP_LOGCONFIG(TAG, "  Role: %s", this->role_.c_str());
  ESP_LOGCONFIG(TAG, "  Firmware channel: %s", this->firmware_channel_.c_str());
  if (!this->uart_baud_.empty()) {
    ESP_LOGCONFIG(TAG, "  UART baud: %s", this->uart_baud_.c_str());
  }
}

void RadioProbe::publish_(const std::string &value) {
  ESP_LOGI(TAG, "installed firmware: %s", value.c_str());
  if (this->installed_ != nullptr) {
    this->installed_->publish_state(value);
  }
}

void RadioProbe::publish_raw_(const std::string &value) {
  // The raw sensor exists so a user can eyeball the untouched wire response
  // when the normalized rev shows "unknown (…)". Only publish when the
  // caller actually has a raw descriptor to share — otherwise leave the
  // sensor unmodified (last known value stays visible in HA).
  ESP_LOGI(TAG, "installed firmware (raw): %s", value.c_str());
  if (this->installed_raw_ != nullptr) {
    this->installed_raw_->publish_state(value);
  }
}

void RadioProbe::drain_rx_() {
  uint8_t discard;
  while (this->available() > 0) {
    if (!this->read_byte(&discard)) {
      break;
    }
  }
}

void RadioProbe::dispatch_() {
  if (this->protocol_ == "none") {
    return;  // radioless board — no sensor emitted
  }

  if (this->protocol_ == "znp") {
    std::string rev, raw;
    if (this->probe_znp_(rev, raw)) {
      this->publish_(rev);
      this->publish_raw_(raw);
    } else {
      // Concrete reason is logged inside probe_znp_(); the sensor gets the
      // generic string so HA's update template can uniformly skip on
      // "unknown".
      this->publish_("unknown (znp probe failed)");
    }
    return;
  }

  if (this->protocol_ == "spinel") {
    std::string rev, raw;
    if (this->probe_spinel_(rev, raw)) {
      this->publish_(rev);
      this->publish_raw_(raw);
    } else {
      this->publish_("unknown (spinel probe failed)");
    }
    return;
  }

  // Remaining v1 stubs — real probes ship in later v1.x PRs per roadmap.md.
  if (this->protocol_ == "ezsp" || this->protocol_ == "zwave") {
    this->publish_("unknown (" + this->protocol_ + " probe not implemented in v1)");
    return;
  }

  ESP_LOGW(TAG, "unknown protocol %s; dispatcher missing branch", this->protocol_.c_str());
  this->publish_("unknown (protocol " + this->protocol_ + " not recognised)");
}

}  // namespace radio_probe
}  // namespace esphome
