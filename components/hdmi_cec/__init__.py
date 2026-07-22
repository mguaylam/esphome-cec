"""HDMI-CEC component for ESPHome, built on the RMT peripheral (esp-idf).

Timing is handed to the RMT hardware: DMA capture on receive, hardware encoding
on transmit. No busy-waiting and no per-bit interrupts, so the component
coexists with a real-time audio pipeline.

This module implements Layer 0 (hub), Layer 1 (named devices) and Layer 2 (the
low-level on_frame trigger and transmit action) of the API described in
DESIGN.md. The semantic triggers and entity platforms (Layers 3-4) and the
device-state registry are not wired up yet.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome import automation, pins
from esphome.const import CONF_ADDRESS, CONF_ID, CONF_PIN, CONF_TRIGGER_ID
from esphome.core import Lambda

CODEOWNERS = ["@mguaylam"]

hdmi_cec_ns = cg.esphome_ns.namespace("hdmi_cec")
HdmiCec = hdmi_cec_ns.class_("HdmiCec", cg.Component)
Frame = hdmi_cec_ns.struct("Frame")
HdmiCecFrameTrigger = hdmi_cec_ns.class_("HdmiCecFrameTrigger", automation.Trigger.template(Frame))
TransmitAction = hdmi_cec_ns.class_("TransmitAction", automation.Action)

# Layer 3 semantic triggers.
KeyTrigger = hdmi_cec_ns.class_("KeyTrigger", automation.Trigger.template(cg.std_string, cg.uint8, cg.uint8))
StandbyTrigger = hdmi_cec_ns.class_("StandbyTrigger", automation.Trigger.template(cg.uint8))
ActiveSourceTrigger = hdmi_cec_ns.class_("ActiveSourceTrigger", automation.Trigger.template(cg.uint16, cg.uint8))
VolumeTrigger = hdmi_cec_ns.class_("VolumeTrigger", automation.Trigger.template(cg.uint8, cg.bool_, cg.uint8))
PowerTrigger = hdmi_cec_ns.class_("PowerTrigger", automation.Trigger.template(cg.bool_, cg.uint8))

# Layer 3 semantic actions.
OpcodeAction = hdmi_cec_ns.class_("OpcodeAction", automation.Action)
KeyPressAction = hdmi_cec_ns.class_("KeyPressAction", automation.Action)
ActiveSourceAction = hdmi_cec_ns.class_("ActiveSourceAction", automation.Action)

DeviceType = hdmi_cec_ns.enum("DeviceType")
DEVICE_TYPES = {
    "tv": DeviceType.DEVICE_TYPE_TV,
    "recorder": DeviceType.DEVICE_TYPE_RECORDER,
    "tuner": DeviceType.DEVICE_TYPE_TUNER,
    "playback": DeviceType.DEVICE_TYPE_PLAYBACK,
    "audio_system": DeviceType.DEVICE_TYPE_AUDIO_SYSTEM,
    "switch": DeviceType.DEVICE_TYPE_SWITCH,
}

# Curated names for the most common CEC opcodes. Anything not listed can still
# be given as a raw hex byte — the low-level layer covers the whole protocol.
OPCODES = {
    "feature_abort": 0x00,
    "image_view_on": 0x04,
    "text_view_on": 0x0D,
    "standby": 0x36,
    "active_source": 0x82,
    "inactive_source": 0x9D,
    "request_active_source": 0x85,
    "routing_change": 0x80,
    "routing_information": 0x81,
    "set_stream_path": 0x86,
    "give_physical_address": 0x83,
    "report_physical_address": 0x84,
    "give_osd_name": 0x46,
    "set_osd_name": 0x47,
    "set_osd_string": 0x64,
    "give_device_vendor_id": 0x8C,
    "device_vendor_id": 0x87,
    "vendor_command": 0x89,
    "give_device_power_status": 0x8F,
    "report_power_status": 0x90,
    "get_cec_version": 0x9F,
    "cec_version": 0x9E,
    "get_menu_language": 0x91,
    "set_menu_language": 0x32,
    "user_control_pressed": 0x44,
    "user_control_released": 0x45,
    "give_audio_status": 0x71,
    "report_audio_status": 0x7A,
    "give_system_audio_mode_status": 0x7D,
    "system_audio_mode_status": 0x7E,
    "set_system_audio_mode": 0x72,
    "system_audio_mode_request": 0x70,
    "give_deck_status": 0x1A,
    "deck_status": 0x1B,
    "deck_control": 0x42,
    "play": 0x41,
}

# The complete CEC User Control Code table, shared by the key triggers and the
# key_press action. Anything not listed can still be given as a raw hex byte.
KEYS = {
    "select": 0x00,
    "up": 0x01,
    "down": 0x02,
    "left": 0x03,
    "right": 0x04,
    "right_up": 0x05,
    "right_down": 0x06,
    "left_up": 0x07,
    "left_down": 0x08,
    "root_menu": 0x09,
    "setup_menu": 0x0A,
    "contents_menu": 0x0B,
    "favorite_menu": 0x0C,
    "exit": 0x0D,
    "media_top_menu": 0x10,
    "media_context_menu": 0x11,
    "number_entry_mode": 0x1D,
    "number_11": 0x1E,
    "number_12": 0x1F,
    "number_0": 0x20,
    "number_1": 0x21,
    "number_2": 0x22,
    "number_3": 0x23,
    "number_4": 0x24,
    "number_5": 0x25,
    "number_6": 0x26,
    "number_7": 0x27,
    "number_8": 0x28,
    "number_9": 0x29,
    "dot": 0x2A,
    "enter": 0x2B,
    "clear": 0x2C,
    "next_favorite": 0x2F,
    "channel_up": 0x30,
    "channel_down": 0x31,
    "previous_channel": 0x32,
    "sound_select": 0x33,
    "input_select": 0x34,
    "display_information": 0x35,
    "help": 0x36,
    "page_up": 0x37,
    "page_down": 0x38,
    "power": 0x40,
    "volume_up": 0x41,
    "volume_down": 0x42,
    "mute": 0x43,
    "play": 0x44,
    "stop": 0x45,
    "pause": 0x46,
    "record": 0x47,
    "rewind": 0x48,
    "fast_forward": 0x49,
    "eject": 0x4A,
    "forward": 0x4B,
    "backward": 0x4C,
    "stop_record": 0x4D,
    "pause_record": 0x4E,
    "angle": 0x50,
    "sub_picture": 0x51,
    "video_on_demand": 0x52,
    "electronic_program_guide": 0x53,
    "timer_programming": 0x54,
    "initial_configuration": 0x55,
    "select_broadcast_type": 0x56,
    "select_sound_presentation": 0x57,
    "play_function": 0x60,
    "pause_play_function": 0x61,
    "record_function": 0x62,
    "pause_record_function": 0x63,
    "stop_function": 0x64,
    "mute_function": 0x65,
    "restore_volume_function": 0x66,
    "tune_function": 0x67,
    "select_media_function": 0x68,
    "select_av_input_function": 0x69,
    "select_audio_input_function": 0x6A,
    "power_toggle_function": 0x6B,
    "power_off_function": 0x6C,
    "power_on_function": 0x6D,
    "f1_blue": 0x71,
    "f2_red": 0x72,
    "f3_green": 0x73,
    "f4_yellow": 0x74,
    "f5": 0x75,
    "data": 0x76,
    # Convenience aliases.
    "next": 0x4B,
    "previous": 0x4C,
    "back": 0x0D,
}

# hdmi_cec.volume action -> User Control Code.
VOLUME_ACTIONS = {"up": 0x41, "down": 0x42, "mute": 0x43}

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
CONF_ON_FRAME = "on_frame"
CONF_OPCODE = "opcode"
CONF_FROM = "from"
CONF_TO = "to"
CONF_PARAMS = "params"
CONF_RAW = "raw"
CONF_ON_KEY_PRESS = "on_key_press"
CONF_ON_KEY_RELEASE = "on_key_release"
CONF_ON_STANDBY = "on_standby"
CONF_ON_ACTIVE_SOURCE = "on_active_source"
CONF_ON_VOLUME = "on_volume"
CONF_ON_POWER = "on_power"
CONF_DEVICE = "device"
CONF_KEY = "key"
CONF_ACTION = "action"

# Sentinel matching PHYSICAL_ADDRESS_NONE in hdmi_cec.h: advertise nothing.
PHYSICAL_ADDRESS_NONE = 0xFFFF

# A single payload byte.
_BYTE = cv.All(cv.hex_int, cv.Range(min=0, max=0xFF))


def _logical_address(value):
    value = cv.hex_int(value)
    if not 0 <= value <= 0xF:
        raise cv.Invalid("A CEC logical address must be between 0x0 and 0xF")
    return value


def _physical_address(value):
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


def _opcode(value):
    if isinstance(value, str):
        key = value.strip().lower()
        if key in OPCODES:
            return OPCODES[key]
    value = cv.hex_int(value)
    if not 0 <= value <= 0xFF:
        raise cv.Invalid("opcode must be a known name or a byte 0x00-0xFF")
    return value


def _filter_address(value):
    # Trigger filter: keywords/name kept as strings, resolved in to_code where
    # the `devices:` map is available; numbers resolved immediately.
    if isinstance(value, str):
        low = value.strip().lower()
        if low in ("us", "broadcast", "any"):
            return low
        try:
            return _logical_address(value)
        except cv.Invalid:
            return low
    return _logical_address(value)


def _action_address(value):
    # Action address: a lambda (templatable), a keyword/device name (resolved at
    # runtime via a token), or a literal number.
    if isinstance(value, Lambda):
        return cv.returning_lambda(value)
    if isinstance(value, str):
        low = value.strip().lower()
        if low in ("us", "broadcast"):
            return low
        try:
            return _logical_address(value)
        except cv.Invalid:
            return low
    return _logical_address(value)


def _key(value):
    if isinstance(value, str):
        k = value.strip().lower()
        if k in KEYS:
            return KEYS[k]
    value = cv.hex_int(value)
    if not 0 <= value <= 0xFF:
        raise cv.Invalid("key must be a known name or a byte 0x00-0xFF")
    return value


# ── Shared by the entity platforms (Layer 4) ─────────────────────────────────
CONF_HDMI_CEC_ID = "hdmi_cec_id"


def entity_device(value):
    # A literal logical address, or a device name resolved against the hub's
    # `devices:` map at runtime.
    if isinstance(value, str):
        try:
            return _logical_address(value)
        except cv.Invalid:
            return cv.valid_name(value)
    return _logical_address(value)


ENTITY_BASE_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_HDMI_CEC_ID): cv.use_id(HdmiCec),
        cv.Required(CONF_DEVICE): entity_device,
    }
)


async def register_entity_device(var, config):
    parent = await cg.get_variable(config[CONF_HDMI_CEC_ID])
    cg.add(var.set_parent(parent))
    device = config[CONF_DEVICE]
    if isinstance(device, int):
        cg.add(var.set_address(device))
    else:
        cg.add(var.set_device_name(device))


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
        cv.Optional(CONF_ON_FRAME): automation.validate_automation(
            {
                cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(HdmiCecFrameTrigger),
                cv.Optional(CONF_OPCODE): _opcode,
                cv.Optional(CONF_FROM): _filter_address,
                cv.Optional(CONF_TO): _filter_address,
            }
        ),
        cv.Optional(CONF_ON_KEY_PRESS): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(KeyTrigger)}
        ),
        cv.Optional(CONF_ON_KEY_RELEASE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(KeyTrigger)}
        ),
        cv.Optional(CONF_ON_STANDBY): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(StandbyTrigger)}
        ),
        cv.Optional(CONF_ON_ACTIVE_SOURCE): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(ActiveSourceTrigger)}
        ),
        cv.Optional(CONF_ON_VOLUME): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(VolumeTrigger)}
        ),
        cv.Optional(CONF_ON_POWER): automation.validate_automation(
            {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(PowerTrigger)}
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _resolve_filter(value, devices):
    # Convert a validated filter value into the int16 the C++ trigger expects:
    # -1 = any, -2 = us, 0..15 = literal (0xF = broadcast).
    if value == "any":
        return -1
    if value == "us":
        return -2
    if value == "broadcast":
        return 0x0F
    if isinstance(value, int):
        return value
    if value in devices:
        return devices[value]
    raise cv.Invalid(f"Unknown device '{value}' referenced in on_frame")


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

    devices = config[CONF_DEVICES]
    for name, addr in devices.items():
        cg.add(var.add_device(name, addr))

    for conf in config.get(CONF_ON_FRAME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        if CONF_OPCODE in conf:
            cg.add(trigger.set_opcode(conf[CONF_OPCODE]))
        if CONF_FROM in conf:
            cg.add(trigger.set_from(_resolve_filter(conf[CONF_FROM], devices)))
        if CONF_TO in conf:
            cg.add(trigger.set_to(_resolve_filter(conf[CONF_TO], devices)))
        await automation.build_automation(trigger, [(Frame, "frame")], conf)

    key_params = [(cg.std_string, "key"), (cg.uint8, "code"), (cg.uint8, "source")]
    for conf in config.get(CONF_ON_KEY_PRESS, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var, False)
        await automation.build_automation(trigger, key_params, conf)
    for conf in config.get(CONF_ON_KEY_RELEASE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var, True)
        await automation.build_automation(trigger, key_params, conf)
    for conf in config.get(CONF_ON_STANDBY, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.uint8, "source")], conf)
    for conf in config.get(CONF_ON_ACTIVE_SOURCE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.uint16, "physical_address"), (cg.uint8, "source")], conf
        )
    for conf in config.get(CONF_ON_VOLUME, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger, [(cg.uint8, "volume"), (cg.bool_, "mute"), (cg.uint8, "source")], conf
        )
    for conf in config.get(CONF_ON_POWER, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(trigger, [(cg.bool_, "on"), (cg.uint8, "source")], conf)


def _validate_transmit(config):
    has_raw = CONF_RAW in config
    has_opcode = CONF_OPCODE in config
    if has_raw and (has_opcode or CONF_PARAMS in config):
        raise cv.Invalid("'raw' cannot be combined with 'opcode' or 'params'")
    if not has_raw and not has_opcode:
        raise cv.Invalid("provide 'opcode' (with optional 'params') or 'raw'")
    return config


TRANSMIT_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.use_id(HdmiCec),
            cv.Required(CONF_TO): _action_address,
            cv.Optional(CONF_FROM): _action_address,
            cv.Optional(CONF_OPCODE): cv.templatable(_opcode),
            cv.Optional(CONF_PARAMS): cv.templatable(cv.ensure_list(_BYTE)),
            cv.Optional(CONF_RAW): cv.templatable(cv.ensure_list(_BYTE)),
        }
    ),
    _validate_transmit,
)


async def _apply_action_address(var, which, value, args):
    if isinstance(value, str):  # "us", "broadcast", or a device-name token
        cg.add(var.set_to_token(value) if which == CONF_TO else var.set_from_token(value))
        return
    tmpl = await cg.templatable(value, args, cg.uint8)
    if which == CONF_TO:
        cg.add(var.set_to(tmpl))
    else:
        cg.add(var.set_source(tmpl))
        cg.add(var.set_has_source(True))


async def _templatable_bytes(value, args):
    if isinstance(value, Lambda):
        return await cg.templatable(value, args, cg.std_vector.template(cg.uint8))
    # A static list: build an explicitly typed vector so the templated setter can
    # deduce its argument (a bare brace-init list cannot be deduced).
    body = ", ".join(f"0x{b:02X}" for b in value)
    return cg.RawExpression(f"std::vector<uint8_t>{{{body}}}")


@automation.register_action("hdmi_cec.transmit", TransmitAction, TRANSMIT_SCHEMA)
async def transmit_action_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)

    await _apply_action_address(var, CONF_TO, config[CONF_TO], args)
    if CONF_FROM in config:
        await _apply_action_address(var, CONF_FROM, config[CONF_FROM], args)

    if CONF_RAW in config:
        cg.add(var.set_raw(await _templatable_bytes(config[CONF_RAW], args)))
        cg.add(var.set_has_raw(True))
    else:
        cg.add(var.set_opcode(await cg.templatable(config[CONF_OPCODE], args, cg.uint8)))
        cg.add(var.set_has_opcode(True))
        if CONF_PARAMS in config:
            cg.add(var.set_params(await _templatable_bytes(config[CONF_PARAMS], args)))
            cg.add(var.set_has_params(True))
    return var


# ── Layer 3 semantic actions ─────────────────────────────────────────────────


async def _apply_device(var, value, args):
    if isinstance(value, str):  # "us", "broadcast", or a device-name token
        cg.add(var.set_device_token(value))
    else:
        cg.add(var.set_device(await cg.templatable(value, args, cg.uint8)))
        cg.add(var.set_has_device(True))


POWER_ON_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(HdmiCec),
        cv.Required(CONF_DEVICE): _action_address,
    }
)


@automation.register_action("hdmi_cec.power_on", OpcodeAction, POWER_ON_SCHEMA)
async def power_on_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent, 0x04)  # Image View On
    await _apply_device(var, config[CONF_DEVICE], args)
    return var


STANDBY_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(HdmiCec),
        cv.Optional(CONF_DEVICE): _action_address,
    }
)


@automation.register_action("hdmi_cec.standby", OpcodeAction, STANDBY_SCHEMA)
async def standby_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent, 0x36)  # Standby
    if CONF_DEVICE in config:
        await _apply_device(var, config[CONF_DEVICE], args)
    return var


KEY_PRESS_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(HdmiCec),
        cv.Required(CONF_DEVICE): _action_address,
        cv.Required(CONF_KEY): cv.templatable(_key),
    }
)


@automation.register_action("hdmi_cec.key_press", KeyPressAction, KEY_PRESS_SCHEMA)
async def key_press_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    await _apply_device(var, config[CONF_DEVICE], args)
    cg.add(var.set_key(await cg.templatable(config[CONF_KEY], args, cg.uint8)))
    return var


VOLUME_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.use_id(HdmiCec),
        cv.Required(CONF_DEVICE): _action_address,
        cv.Required(CONF_ACTION): cv.enum(VOLUME_ACTIONS, lower=True),
    }
)


@automation.register_action("hdmi_cec.volume", KeyPressAction, VOLUME_SCHEMA)
async def volume_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    await _apply_device(var, config[CONF_DEVICE], args)
    cg.add(var.set_key(config[CONF_ACTION]))  # already a User Control Code
    return var


ACTIVE_SOURCE_SCHEMA = cv.Schema({cv.GenerateID(): cv.use_id(HdmiCec)})


@automation.register_action("hdmi_cec.active_source", ActiveSourceAction, ACTIVE_SOURCE_SCHEMA)
async def active_source_to_code(config, action_id, template_arg, args):
    parent = await cg.get_variable(config[CONF_ID])
    var = cg.new_Pvariable(action_id, template_arg, parent)
    return var
