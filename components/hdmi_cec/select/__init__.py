import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import select
from esphome.const import CONF_OPTIONS

from .. import ENTITY_BASE_SCHEMA, hdmi_cec_ns, register_entity_device

DEPENDENCIES = ["hdmi_cec"]

HdmiCecSelect = hdmi_cec_ns.class_("HdmiCecSelect", select.Select, cg.PollingComponent)

_PHYSICAL_ADDRESS = cv.All(cv.hex_int, cv.Range(min=0, max=0xFFFF))

CONFIG_SCHEMA = (
    select.select_schema(HdmiCecSelect)
    .extend(ENTITY_BASE_SCHEMA)
    .extend(cv.polling_component_schema("1s"))
    .extend(
        {
            cv.Required(CONF_OPTIONS): cv.All(
                cv.Schema({cv.string_strict: _PHYSICAL_ADDRESS}),
                cv.Length(min=1),
            ),
        }
    )
)


async def to_code(config):
    options = config[CONF_OPTIONS]
    var = await select.new_select(config, options=list(options.keys()))
    await cg.register_component(var, config)
    await register_entity_device(var, config)
    for label, physical_address in options.items():
        cg.add(var.add_option(label, physical_address))
