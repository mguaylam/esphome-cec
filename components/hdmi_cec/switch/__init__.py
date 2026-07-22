import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import switch

from .. import ENTITY_BASE_SCHEMA, hdmi_cec_ns, register_entity_device

DEPENDENCIES = ["hdmi_cec"]

HdmiCecSwitch = hdmi_cec_ns.class_("HdmiCecSwitch", switch.Switch, cg.PollingComponent)

CONFIG_SCHEMA = (
    switch.switch_schema(HdmiCecSwitch)
    .extend(ENTITY_BASE_SCHEMA)
    .extend(cv.polling_component_schema("1s"))
)


async def to_code(config):
    var = await switch.new_switch(config)
    await cg.register_component(var, config)
    await register_entity_device(var, config)
