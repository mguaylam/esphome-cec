import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor
from esphome.const import CONF_TYPE, UNIT_PERCENT

from .. import ENTITY_BASE_SCHEMA, hdmi_cec_ns, register_entity_device

DEPENDENCIES = ["hdmi_cec"]

HdmiCecSensor = hdmi_cec_ns.class_("HdmiCecSensor", sensor.Sensor, cg.PollingComponent)

CONFIG_SCHEMA = (
    sensor.sensor_schema(
        HdmiCecSensor, unit_of_measurement=UNIT_PERCENT, accuracy_decimals=0
    )
    .extend(ENTITY_BASE_SCHEMA)
    .extend(cv.polling_component_schema("1s"))
    .extend(
        {
            cv.Optional(CONF_TYPE, default="audio_volume"): cv.one_of(
                "audio_volume", lower=True
            )
        }
    )
)


async def to_code(config):
    var = await sensor.new_sensor(config)
    await cg.register_component(var, config)
    await register_entity_device(var, config)
