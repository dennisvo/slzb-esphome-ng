// UART hardware flow control (CTS/RTS) — implementation.
//
// Calls ESP-IDF `uart_set_pin()` to route CTS/RTS through the GPIO matrix,
// then `uart_set_hw_flow_ctrl()` to enable CTS/RTS handshake on the ESP32
// UART peripheral. About 20 lines of real work — the rest is guard-rails.
//
// Design authority: docs/v1-design.md §5.

#include "uart_hw_flow.h"

#include "esphome/core/log.h"

#ifdef USE_ESP_IDF
#include "driver/uart.h"

#include "esphome/components/uart/uart_component_esp_idf.h"
#endif

namespace esphome {
namespace uart_hw_flow {

void UartHwFlow::setup() {
#ifdef USE_ESP_IDF
  if (!this->enabled_) {
    // Matches stock SMLIGHT radio firmware default (hwFlow=false) on MR4U.
    // Deliberate no-op keeps YAML shape stable across enabled/disabled.
    return;
  }

  if (this->parent_ == nullptr) {
    ESP_LOGE(TAG, "No uart_id parent set; cannot enable HW flow control");
    this->mark_failed();
    return;
  }

  // Under esp-idf framework the only UARTComponent implementation is
  // IDFUARTComponent; static_cast is safe. The base class (uart_component.h)
  // doesn't expose the ESP-IDF port number, so we have to reach into the
  // concrete type.
  auto *idf = static_cast<uart::IDFUARTComponent *>(this->parent_);
  const uart_port_t port = static_cast<uart_port_t>(idf->get_hw_serial_number());

  esp_err_t rc = uart_set_pin(port, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                              this->rts_pin_ >= 0 ? this->rts_pin_ : UART_PIN_NO_CHANGE,
                              this->cts_pin_ >= 0 ? this->cts_pin_ : UART_PIN_NO_CHANGE);
  if (rc != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_pin(uart%d) failed: %d", port, rc);
    this->mark_failed();
    return;
  }

  rc = uart_set_hw_flow_ctrl(port, UART_HW_FLOWCTRL_CTS_RTS, RX_FLOW_THRESHOLD);
  if (rc != ESP_OK) {
    ESP_LOGE(TAG, "uart_set_hw_flow_ctrl(uart%d) failed: %d", port, rc);
    this->mark_failed();
    return;
  }

  ESP_LOGCONFIG(TAG, "UART%d HW flow control enabled (CTS=GPIO%d, RTS=GPIO%d, threshold=%u)", port, this->cts_pin_,
                this->rts_pin_, RX_FLOW_THRESHOLD);
#else
  // Arduino framework path — no ESP-IDF UART driver to talk to. Fork ships
  // ESP-IDF only, so this branch should never fire in normal builds.
  ESP_LOGW(TAG, "uart_hw_flow is a no-op outside ESP-IDF");
#endif
}

void UartHwFlow::dump_config() {
  ESP_LOGCONFIG(TAG, "UART HW Flow Control:");
  ESP_LOGCONFIG(TAG, "  Enabled: %s", this->enabled_ ? "yes" : "no");
  ESP_LOGCONFIG(TAG, "  CTS pin: GPIO%d", this->cts_pin_);
  ESP_LOGCONFIG(TAG, "  RTS pin: GPIO%d", this->rts_pin_);
}

}  // namespace uart_hw_flow
}  // namespace esphome
