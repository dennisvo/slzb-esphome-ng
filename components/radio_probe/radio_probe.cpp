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

#include <cstdarg>
#include <cstdio>

#include "esphome/core/log.h"

namespace esphome {
namespace radio_probe {

void RadioProbe::trace_(const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n <= 0) {
    return;
  }
  if (static_cast<size_t>(n) >= sizeof(buf)) {
    n = sizeof(buf) - 1;
  }
  // Cap total trace at ~1 KB so runaway traces can't eat all RAM.
  if (this->debug_trace_.size() > 1024) {
    return;
  }
  this->debug_trace_.append(buf, static_cast<size_t>(n));
  this->debug_trace_.push_back('\n');
}

void RadioProbe::setup() {
  this->setup_ran_ = true;
  this->trace_("setup: protocol=%s chip=%s", this->protocol_.c_str(), this->chip_.c_str());
  this->dispatch_();
  this->trace_("setup: exit");
}

void RadioProbe::dump_config() {
  ESP_LOGE(TAG, "[DEBUG] protocol=%s setup_ran=%d component_state=0x%02X",
           this->protocol_.c_str(),
           this->setup_ran_ ? 1 : 0,
           static_cast<unsigned>(this->get_component_state()));
  // Replay the setup-time breadcrumb buffer line-by-line at ERROR level so it
  // survives ESPHome's early-boot ring-buffer eviction (see repo memory:
  // esphome-log-buffer.md).
  size_t start = 0;
  while (start < this->debug_trace_.size()) {
    size_t nl = this->debug_trace_.find('\n', start);
    if (nl == std::string::npos) {
      nl = this->debug_trace_.size();
    }
    ESP_LOGE(TAG, "[TRACE] %.*s", static_cast<int>(nl - start),
             this->debug_trace_.c_str() + start);
    start = nl + 1;
  }
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
  this->trace_("dispatch: protocol=%s", this->protocol_.c_str());
  if (this->protocol_ == "none") {
    return;  // radioless board — no sensor emitted
  }

  if (this->protocol_ == "znp") {
    std::string result;
    if (this->probe_znp_(result)) {
      this->trace_("znp: OK '%s'", result.c_str());
      this->publish_(result);
    } else {
      this->trace_("znp: FAIL");
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
      this->trace_("spinel: OK '%s'", result.c_str());
      this->publish_(result);
    } else {
      this->trace_("spinel: FAIL");
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
