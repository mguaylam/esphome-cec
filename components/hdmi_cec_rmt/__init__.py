"""HDMI-CEC component for ESPHome, built on the RMT peripheral (esp-idf).

Unlike implementations that bit-bang the bus from an ISR using busy-waits, this
one hands timing to the RMT hardware: DMA capture on receive, hardware encoding
on transmit. No busy-waiting and no per-bit interrupts, so the component
coexists with a real-time audio pipeline (validated with Sendspin/Music
Assistant on an ESP32-S3).

Capabilities: frame reception and decoding, transmission, receiver ACK.
Known limitations (see ROADMAP.md): no collision arbitration, no retransmission
after a NACK, and the logical address is fixed at compile time.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ID, CONF_PIN

CODEOWNERS = ["@mguaylam"]

hdmi_cec_rmt_ns = cg.esphome_ns.namespace("hdmi_cec_rmt")
HdmiCecRmt = hdmi_cec_rmt_ns.class_("HdmiCecRmt", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HdmiCecRmt),
        cv.Required(CONF_PIN): pins.internal_gpio_input_pin_number,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pin(config[CONF_PIN]))
