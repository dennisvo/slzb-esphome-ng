// Radio firmware version probe — dispatcher.
//
// The v1 dispatcher is a switch on the `protocol` substitution:
//   znp     -> live ZNP SYS_VERSION probe (znp_probe.cpp)
//   spinel  -> live Spinel PROP_VALUE_GET(NCP_VERSION) probe (spinel_probe.cpp)
//   ezsp    -> stub (deferred to v1.x per roadmap.md)
//   zwave   -> stub (deferred to v1.x)
//   none    -> no sensor emitted (radioless board)
//
// See docs/v1-radio-firmware.md §§5, 6d, 6f.

#include "radio_probe.h"

#include "esphome/core/log.h"

namespace esphome {
namespace radio_probe {

void RadioProbe::setup() { this->dispatch_(); }

void RadioProbe::dump_config() {
  ESP_LOGCONFIG(TAG, "Radio Probe:");
  ESP_LOGCONFIG(TAG, "  Chip: %s", this->chip_.c_str());
  ESP_LOGCONFIG(TAG, "  SMLIGHT id: %s", this->smlight_id_.c_str());
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
    std::string result;
    if (this->probe_znp_(result)) {
      this->publish_(result);
    } else {
      // Concrete reason is logged inside probe_znp_(); the sensor gets the
      // generic string so HA's update template can uniformly skip on
      // "unknown".
      this->publish_("unknown (znp probe failed)");
    }
    return;
  }

  if (this->protocol_ == "spinel") {
    std::string result;
    if (this->probe_spinel_(result)) {
      this->publish_(result);
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
