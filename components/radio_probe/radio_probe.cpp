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

// Publishes chip_probed / role_probed and logs any mismatch against the
// declared (YAML) chip / role. Mismatch state is log-only by design (§6i):
// HA re-derives whatever verdict it needs from the two sensors + the fetched
// catalog, so the device doesn't overclaim knowledge it doesn't have.
void RadioProbe::publish_probed_(const std::string &chip_probed, const std::string &role_probed,
                                 bool probe_ok) {
  const std::string chip_val = probe_ok && !chip_probed.empty() ? chip_probed : "unknown";
  const std::string role_val = probe_ok && !role_probed.empty() ? role_probed : "unknown";
  if (this->chip_probed_ != nullptr) {
    this->chip_probed_->publish_state(chip_val);
  }
  if (this->role_probed_ != nullptr) {
    this->role_probed_->publish_state(role_val);
  }

  if (!probe_ok) {
    return;  // probe already logged its own failure reason
  }

  // Chip mismatch verdict — three-valued, log-only. "family_match" covers
  // the ZNP CC26xx case where the wire can only confirm the family, not
  // the specific variant declared in YAML.
  if (!chip_probed.empty() && chip_probed != this->chip_) {
    const bool cc26xx_family_ok = chip_probed == "cc26xx_family" &&
                                  (this->chip_ == "cc2674p10" || this->chip_ == "cc1352p7" ||
                                   this->chip_ == "cc1352p2");
    const bool efr32_family_ok = chip_probed == "efr32" &&
                                 (this->chip_ == "efr32mg26" || this->chip_ == "efr32mg24");
    if (cc26xx_family_ok || efr32_family_ok) {
      ESP_LOGW(TAG, "chip: configured=%s probed=%s (family match — variant unconfirmed by wire)",
               this->chip_.c_str(), chip_probed.c_str());
    } else {
      ESP_LOGE(TAG, "chip mismatch: configured=%s probed=%s "
                    "(wire evidence contradicts hw_defs — check hardware)",
               this->chip_.c_str(), chip_probed.c_str());
    }
  }

  if (!role_probed.empty() && role_probed != this->role_) {
    ESP_LOGE(TAG, "role mismatch: configured=%s probed=%s", this->role_.c_str(),
             role_probed.c_str());
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
    std::string rev, raw, chip_probed, role_probed;
    const bool ok = this->probe_znp_(rev, raw, chip_probed, role_probed);
    if (ok) {
      this->publish_(rev);
      this->publish_raw_(raw);
    } else {
      // Concrete reason is logged inside probe_znp_(); the sensor gets the
      // generic string so HA's update template can uniformly skip on
      // "unknown".
      this->publish_("unknown (znp probe failed)");
    }
    this->publish_probed_(chip_probed, role_probed, ok);
    return;
  }

  if (this->protocol_ == "spinel") {
    std::string rev, raw, chip_probed, role_probed;
    const bool ok = this->probe_spinel_(rev, raw, chip_probed, role_probed);
    if (ok) {
      this->publish_(rev);
      this->publish_raw_(raw);
    } else {
      this->publish_("unknown (spinel probe failed)");
    }
    this->publish_probed_(chip_probed, role_probed, ok);
    return;
  }

  // Remaining v1 stubs — real probes ship in later v1.x PRs per roadmap.md.
  if (this->protocol_ == "ezsp" || this->protocol_ == "zwave") {
    this->publish_("unknown (" + this->protocol_ + " probe not implemented in v1)");
    this->publish_probed_("", "", false);
    return;
  }

  ESP_LOGW(TAG, "unknown protocol %s; dispatcher missing branch", this->protocol_.c_str());
  this->publish_("unknown (protocol " + this->protocol_ + " not recognised)");
  this->publish_probed_("", "", false);
}

}  // namespace radio_probe
}  // namespace esphome
