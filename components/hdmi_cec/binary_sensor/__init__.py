import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor
from esphome.const import CONF_TYPE

from .. import ENTITY_BASE_SCHEMA, hdmi_cec_ns, register_entity_device

DEPENDENCIES = ["hdmi_cec"]

HdmiCecBinarySensor = hdmi_cec_ns.class_(
    "HdmiCecBinarySensor", binary_sensor.BinarySensor, cg.PollingComponent
)

BinaryType = hdmi_cec_ns.enum("HdmiCecBinaryType")
BINARY_TYPES = {
    "mute": BinaryType.HDMI_CEC_BINARY_MUTE,
    "power": BinaryType.HDMI_CEC_BINARY_POWER,
}

CONFIG_SCHEMA = (
    binary_sensor.binary_sensor_schema(HdmiCecBinarySensor)
    .extend(ENTITY_BASE_SCHEMA)
    .extend(cv.polling_component_schema("1s"))
    .extend({cv.Required(CONF_TYPE): cv.enum(BINARY_TYPES, lower=True)})
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    await register_entity_device(var, config)
    cg.add(var.set_type(config[CONF_TYPE]))
