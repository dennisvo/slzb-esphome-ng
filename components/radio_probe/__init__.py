"""Radio firmware version probe component for slzb-esphome-ng.

v1: dispatches on ``protocol`` to a live probe (ZNP ``SYS_VERSION``) or
per-protocol stub. Publishes the parsed installed firmware version
(YYYYMMDD string for ZNP, or ``"unknown (<reason>)"``) to a text sensor.

Design authority: ``docs/v1-radio-firmware.md`` §§3, 5, 6a, 6d, 6g.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor, uart
from esphome.const import CONF_ID

CODEOWNERS = ["@dennisvo"]
DEPENDENCIES = ["uart"]
AUTO_LOAD = ["text_sensor"]
MULTI_CONF = True

CONF_CHIP = "chip"
CONF_SMLIGHT_ID = "smlight_id"
CONF_PROTOCOL = "protocol"
CONF_ROLE = "role"
CONF_FIRMWARE_CHANNEL = "firmware_channel"
CONF_UART_BAUD = "uart_baud"
CONF_INSTALLED_FIRMWARE = "installed_firmware"

# v1-radio-firmware.md §6g. "none" is the radioless-board sentinel (slw09u).
CHIPS = ["cc2674p10", "cc1352p7", "efr32mg26", "efr32mg24", "zw800", "none"]
PROTOCOLS = ["znp", "spinel", "ezsp", "zwave", "none"]
ROLES = ["coord", "router", "rcp", "ncp", "primary_ctrl", "none"]
CHANNELS = ["prod", "dev"]

# (chip, protocol, role) triples that map to a real SMLIGHT catalog entry.
# See v1-radio-firmware.md §6g — anything not in this set is rejected at
# `esphome config` time.
VALID_TRIPLES = frozenset(
    {
        ("cc2674p10", "znp", "coord"),
        ("cc2674p10", "znp", "router"),
        ("cc1352p7", "znp", "coord"),
        ("cc1352p7", "znp", "router"),
        ("efr32mg26", "ezsp", "coord"),
        ("efr32mg26", "ezsp", "router"),
        ("efr32mg26", "spinel", "rcp"),
        ("efr32mg26", "spinel", "ncp"),
        ("efr32mg24", "ezsp", "coord"),
        ("efr32mg24", "ezsp", "router"),
        ("efr32mg24", "spinel", "rcp"),
        ("zw800", "zwave", "primary_ctrl"),
        ("none", "none", "none"),
    }
)

radio_probe_ns = cg.esphome_ns.namespace("radio_probe")
RadioProbe = radio_probe_ns.class_("RadioProbe", cg.Component, uart.UARTDevice)


def _validate_triple(config):
    triple = (config[CONF_CHIP], config[CONF_PROTOCOL], config[CONF_ROLE])
    if triple not in VALID_TRIPLES:
        raise cv.Invalid(
            f"radio_probe: invalid (chip, protocol, role) triple "
            f"({triple[0]!r}, {triple[1]!r}, {triple[2]!r}). "
            "See docs/v1-radio-firmware.md §6g for the permutation matrix."
        )
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(RadioProbe),
            cv.Required(CONF_CHIP): cv.one_of(*CHIPS, lower=True),
            cv.Optional(CONF_SMLIGHT_ID, default=""): cv.string,
            cv.Required(CONF_PROTOCOL): cv.one_of(*PROTOCOLS, lower=True),
            cv.Required(CONF_ROLE): cv.one_of(*ROLES, lower=True),
            cv.Optional(CONF_FIRMWARE_CHANNEL, default="dev"): cv.one_of(
                *CHANNELS, lower=True
            ),
            cv.Optional(CONF_UART_BAUD, default=""): cv.string,
            cv.Optional(CONF_INSTALLED_FIRMWARE): text_sensor.text_sensor_schema(),
        }
    )
    .extend(uart.UART_DEVICE_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA),
    _validate_triple,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await uart.register_uart_device(var, config)

    cg.add(var.set_chip(config[CONF_CHIP]))
    cg.add(var.set_smlight_id(config[CONF_SMLIGHT_ID]))
    cg.add(var.set_protocol(config[CONF_PROTOCOL]))
    cg.add(var.set_role(config[CONF_ROLE]))
    cg.add(var.set_firmware_channel(config[CONF_FIRMWARE_CHANNEL]))
    cg.add(var.set_uart_baud(config[CONF_UART_BAUD]))

    if CONF_INSTALLED_FIRMWARE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_INSTALLED_FIRMWARE])
        cg.add(var.set_installed_firmware_sensor(sens))
