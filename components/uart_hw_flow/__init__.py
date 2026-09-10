"""UART hardware flow control (CTS/RTS) for slzb-esphome-ng.

Wraps ESP-IDF ``uart_set_hw_flow_ctrl()`` + ``uart_set_pin()`` to enable
classic RTS/CTS handshake on an ESPHome ``uart:`` bus. Upstream ESPHome
exposes no equivalent YAML — ``uart.flow_control_pin`` is for RS485
driver-enable, not for hardware handshake.

The component always instantiates; ``enabled: false`` makes ``setup()``
a no-op. This keeps the YAML surface stable regardless of the boolean
value in ``hw_defs/**`` and lets ``dump_config`` report the state.

Design authority: ``docs/v1-design.md`` §5.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.components import uart
from esphome.const import CONF_ID

CODEOWNERS = ["@dennisvo"]
DEPENDENCIES = ["uart"]
MULTI_CONF = True

CONF_UART_ID = "uart_id"
CONF_CTS_PIN = "cts_pin"
CONF_RTS_PIN = "rts_pin"
CONF_ENABLED = "enabled"

uart_hw_flow_ns = cg.esphome_ns.namespace("uart_hw_flow")
UartHwFlow = uart_hw_flow_ns.class_("UartHwFlow", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(UartHwFlow),
        cv.Required(CONF_UART_ID): cv.use_id(uart.UARTComponent),
        cv.Required(CONF_CTS_PIN): pins.internal_gpio_input_pin_number,
        cv.Required(CONF_RTS_PIN): pins.internal_gpio_output_pin_number,
        cv.Optional(CONF_ENABLED, default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    parent = await cg.get_variable(config[CONF_UART_ID])
    cg.add(var.set_parent(parent))
    cg.add(var.set_cts_pin(config[CONF_CTS_PIN]))
    cg.add(var.set_rts_pin(config[CONF_RTS_PIN]))
    cg.add(var.set_enabled(config[CONF_ENABLED]))
