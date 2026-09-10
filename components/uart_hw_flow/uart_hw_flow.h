// UART hardware flow control (CTS/RTS) — header.
//
// Runs at setup_priority 250 — after the ESP-IDF UART driver is initialized
// by esphome::uart (~1000) and before serial_proxy attaches at
// AFTER_CONNECTION (~-30). Same slot as radio_probe; they touch different
// state and don't interfere.
//
// Design authority: docs/v1-design.md §5.

#pragma once

#include "esphome/components/uart/uart.h"
#include "esphome/core/component.h"

namespace esphome {
namespace uart_hw_flow {

static const char *const TAG = "uart_hw_flow";

// ESP32 UART FIFO is 128 bytes; assert RTS (stop-sending) when the RX FIFO
// hits this fill level. 122 leaves 6 bytes of in-flight headroom, matching
// SLZB-OS and common ESP-IDF examples.
static const uint8_t RX_FLOW_THRESHOLD = 122;

class UartHwFlow : public Component {
 public:
  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override { return 250.0f; }

  void set_parent(uart::UARTComponent *parent) { this->parent_ = parent; }
  void set_cts_pin(int p) { this->cts_pin_ = p; }
  void set_rts_pin(int p) { this->rts_pin_ = p; }
  void set_enabled(bool e) { this->enabled_ = e; }

 protected:
  uart::UARTComponent *parent_{nullptr};
  int cts_pin_{-1};
  int rts_pin_{-1};
  bool enabled_{false};
};

}  // namespace uart_hw_flow
}  // namespace esphome
