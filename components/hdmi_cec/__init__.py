"""HDMI-CEC component for ESPHome, built on the RMT peripheral (esp-idf).

Timing is handed to the RMT hardware: DMA capture on receive, hardware encoding
on transmit. No busy-waiting and no per-bit interrupts, so the component
coexists with a real-time audio pipeline.

This module implements Layer 0 (the hub) and Layer 1 (named devices) of the API
described in DESIGN.md. The higher layers (on_frame/transmit, semantic triggers
and entity platforms) are not wired up yet.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import pins
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_PIN

CODEOWNERS = ["@mguaylam"]

hdmi_cec_ns = cg.esphome_ns.namespace("hdmi_cec")
HdmiCec = hdmi_cec_ns.class_("HdmiCec", cg.Component)

DeviceType = hdmi_cec_ns.enum("DeviceType")
DEVICE_TYPES = {
    "tv": DeviceType.DEVICE_TYPE_TV,
    "recorder": DeviceType.DEVICE_TYPE_RECORDER,
    "tuner": DeviceType.DEVICE_TYPE_TUNER,
    "playback": DeviceType.DEVICE_TYPE_PLAYBACK,
    "audio_system": DeviceType.DEVICE_TYPE_AUDIO_SYSTEM,
    "switch": DeviceType.DEVICE_TYPE_SWITCH,
}

CONF_DEVICE_TYPE = "device_type"
CONF_PHYSICAL_ADDRESS = "physical_address"
CONF_OSD_NAME = "osd_name"
CONF_VENDOR_ID = "vendor_id"
CONF_AUTO_RESPOND = "auto_respond"
CONF_MONITOR_MODE = "monitor_mode"
CONF_PROMISCUOUS_MODE = "promiscuous_mode"
CONF_RETRANSMIT = "retransmit"
CONF_UPDATE_INTERVAL = "update_interval"
CONF_DEVICES = "devices"

# Sentinel matching PHYSICAL_ADDRESS_NONE in hdmi_cec.h: advertise nothing.
PHYSICAL_ADDRESS_NONE = 0xFFFF


def _logical_address(value):
    value = cv.hex_int(value)
    if not 0 <= value <= 0xF:
        raise cv.Invalid("A CEC logical address must be between 0x0 and 0xF")
    return value


def _physical_address(value):
    # Either the literal "none" (advertise nothing) or a packed 16-bit value
    # such as 0x1000, whose nibbles are the a.b.c.d HDMI topology position.
    if isinstance(value, str) and value.strip().lower() == "none":
        return PHYSICAL_ADDRESS_NONE
    value = cv.hex_int(value)
    if not 0 <= value <= 0xFFFF:
        raise cv.Invalid("A CEC physical address is a 16-bit value, e.g. 0x1000, or 'none'")
    if value == PHYSICAL_ADDRESS_NONE:
        raise cv.Invalid("0xFFFF is reserved; use 'none' to advertise nothing")
    return value


def _devices(value):
    if not isinstance(value, dict):
        raise cv.Invalid("devices must be a mapping of name -> logical address")
    out = {}
    for name, addr in value.items():
        name = cv.valid_name(name)
        if name in out:
            raise cv.Invalid(f"Duplicate device name '{name}'")
        out[name] = _logical_address(addr)
    return out


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(HdmiCec),
        cv.Required(CONF_PIN): pins.internal_gpio_input_pin_number,
        cv.Optional(CONF_DEVICE_TYPE, default="playback"): cv.enum(DEVICE_TYPES, lower=True),
        # Optional explicit logical address; when set it skips negotiation.
        cv.Optional(CONF_ADDRESS): _logical_address,
        cv.Optional(CONF_PHYSICAL_ADDRESS, default="none"): _physical_address,
        cv.Optional(CONF_OSD_NAME, default="ESPHome"): cv.All(cv.string, cv.Length(max=14)),
        cv.Optional(CONF_VENDOR_ID): cv.All(cv.hex_int, cv.Range(min=0, max=0xFFFFFF)),
        cv.Optional(CONF_AUTO_RESPOND, default=True): cv.boolean,
        cv.Optional(CONF_MONITOR_MODE, default=False): cv.boolean,
        cv.Optional(CONF_PROMISCUOUS_MODE, default=False): cv.boolean,
        cv.Optional(CONF_RETRANSMIT, default=5): cv.int_range(min=0, max=5),
        cv.Optional(CONF_UPDATE_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
        cv.Optional(CONF_DEVICES, default={}): _devices,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_pin(config[CONF_PIN]))
    cg.add(var.set_device_type(config[CONF_DEVICE_TYPE]))
    if CONF_ADDRESS in config:
        cg.add(var.set_address_override(config[CONF_ADDRESS]))
    cg.add(var.set_physical_address(config[CONF_PHYSICAL_ADDRESS]))
    cg.add(var.set_osd_name(config[CONF_OSD_NAME]))
    if CONF_VENDOR_ID in config:
        cg.add(var.set_vendor_id(config[CONF_VENDOR_ID]))
    cg.add(var.set_auto_respond(config[CONF_AUTO_RESPOND]))
    cg.add(var.set_monitor_mode(config[CONF_MONITOR_MODE]))
    cg.add(var.set_promiscuous_mode(config[CONF_PROMISCUOUS_MODE]))
    cg.add(var.set_retransmit(config[CONF_RETRANSMIT]))
    cg.add(var.set_update_interval(config[CONF_UPDATE_INTERVAL].total_milliseconds))
    for name, addr in config[CONF_DEVICES].items():
        cg.add(var.add_device(name, addr))
