import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import text_sensor
from esphome.const import CONF_TYPE

from .. import ENTITY_BASE_SCHEMA, hdmi_cec_ns, register_entity_device

DEPENDENCIES = ["hdmi_cec"]

HdmiCecTextSensor = hdmi_cec_ns.class_(
    "HdmiCecTextSensor", text_sensor.TextSensor, cg.PollingComponent
)

TextType = hdmi_cec_ns.enum("HdmiCecTextType")
TEXT_TYPES = {
    "osd_name": TextType.HDMI_CEC_TEXT_OSD_NAME,
    "power_state": TextType.HDMI_CEC_TEXT_POWER_STATE,
}

CONFIG_SCHEMA = (
    text_sensor.text_sensor_schema(HdmiCecTextSensor)
    .extend(ENTITY_BASE_SCHEMA)
    .extend(cv.polling_component_schema("1s"))
    .extend({cv.Required(CONF_TYPE): cv.enum(TEXT_TYPES, lower=True)})
)


async def to_code(config):
    var = await text_sensor.new_text_sensor(config)
    await cg.register_component(var, config)
    await register_entity_device(var, config)
    cg.add(var.set_type(config[CONF_TYPE]))
