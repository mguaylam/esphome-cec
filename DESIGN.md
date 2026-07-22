# Design — the `hdmi_cec` component API

The target API, designed from scratch and free of implementation baggage. The RMT
peripheral, the receiver ACK, the double buffering — none of it appears here. The
YAML describes *what the component does on the bus*, never *how the ESP32 drives
the wire*.

This document is the contract. The implementation (currently RMT-based) must
conform to it, not the other way around.

## Guiding principles

1. **The peripheral is invisible.** The public name is `hdmi_cec:`. No `_rmt`
   anywhere a user can see it.
2. **Clean sheet.** The API is designed to be the best it can be, on its own
   terms. Every layer — including the low-level one (§3) — speaks in names, not
   magic numbers.
3. **Hide the protocol, but never lock it away.** High-level layers speak in
   power/volume/input/keys. The low-level `on_frame` / `transmit` escape hatch
   always remains for anything the abstraction doesn't cover.
4. **Correct by default.** Address negotiation, collision arbitration and NACK
   retransmission are on, not opt-in. Advertising a physical address is off by
   default (an unnegotiated physical address is suspected of destabilising some
   amplifiers).

---

## Layer 0 — the hub

```yaml
hdmi_cec:
  id: bus
  pin: GPIO4                     # required

  # ── Identity ──────────────────────────────────────────────
  device_type: playback          # tv | recorder | tuner | playback | audio_system | switch
                                 # drives logical-address negotiation (see below)
  address: 0x8                   # optional override; skips negotiation (Palakis-compat)
  physical_address: none         # none (default) | 0x1000 | auto (future: read via DDC)
  osd_name: "ESPHome"            # advertised on Give OSD Name; <= 14 chars
  vendor_id: 0x000000            # optional; advertised on Give Device Vendor ID

  # ── Behaviour ─────────────────────────────────────────────
  auto_respond: true             # answer standard queries by itself (see §Housekeeping)
  monitor_mode: false            # never drive the line; listen only (implies address 0xF)
  promiscuous_mode: false        # fire triggers for frames not addressed to us

  # ── Reliability ───────────────────────────────────────────
  retransmit: 5                  # attempts on NACK (spec allows up to 5); 0 = fire and forget
                                 # collision arbitration is always on and not configurable
  update_interval: 30s           # active-poll cadence for the device registry (§Architecture)
```

### Logical-address negotiation

`device_type` selects a pool of logical addresses. At startup the component polls
each address in the pool (zero-length frame to itself; an ACK means taken) and
claims the first free one. A bus with the address already occupied no longer
produces a silent conflict.

| `device_type`  | Address pool           |
|----------------|------------------------|
| `tv`           | 0x0                    |
| `recorder`     | 0x1, 0x2, 0x9          |
| `tuner`        | 0x3, 0x6, 0x7, 0xA     |
| `playback`     | 0x4, 0x8, 0xB          |
| `audio_system` | 0x5                    |
| `switch`       | 0xF (unregistered)     |

An explicit `address:` overrides negotiation entirely (Palakis compatibility).

### Housekeeping (`auto_respond: true`)

The component answers, on its own, the queries the spec requires every device to
handle — so the user never wires these by hand:

- Give OSD Name → **Set OSD Name** (`osd_name`)
- Give Physical Address → **Report Physical Address** (only if one is configured)
- Give Device Power Status → **Report Power Status**
- Give Device Vendor ID → **Device Vendor ID** (or Feature Abort if unset)
- Get CEC Version → **CEC Version** (1.4)
- `<Polling message>` (zero-length) → ACK only

Directly-addressed messages that are neither handled nor broadcast get a
**Feature Abort** (never for broadcasts). Set `auto_respond: false` to take full
manual control via §3.

---

## Layer 1 — named devices

Give the logical addresses on your bus readable names. Every place that expects
an address (`source`, `destination`, `device`) then accepts the name **or** the
raw number.

```yaml
hdmi_cec:
  devices:
    tv: 0x0
    avr: 0x5
    apple_tv: 0x4
```

Names are resolved at code-generation time to their numeric address; there is no
runtime cost. Numbers keep working everywhere, so Palakis configs pasted verbatim
still validate.

---

## Layer 2 — low-level (clean sheet)

The universal escape hatch, for any opcode the higher layers don't model. It
speaks in **named opcodes** and a **structured frame object**, so even a
low-level config stays self-documenting — no magic numbers required.

**Coverage guarantee.** `on_frame` matches *any* opcode (named or raw hex) and
`transmit` sends *any* opcode, so the entire CEC protocol — present and future,
standard and vendor — is reachable from YAML. The semantic triggers (Layer 3)
and entity platforms (Layer 4) are conveniences layered over this complete
foundation;
they are never a ceiling. Whatever a user imagines doing on the bus, this layer
can express it.

An opcode is written as its CEC name (`report_audio_status`) or, for anything not
in the shipped name table, as a raw hex byte (`0x7A`). Same for addresses: a
device name, a number, or the keywords `us` / `broadcast` / `any`.

### Trigger `on_frame`

```yaml
hdmi_cec:
  on_frame:
    - opcode: report_audio_status   # name or 0x7A; omit → any opcode
      from: avr                     # name | number | us | any; omit → any source
      to: us                        # name | number | us | broadcast | any
      then:
        - lambda: |-
            // exposed as a single `frame` struct:
            //   uint8_t  frame.from
            //   uint8_t  frame.to
            //   uint8_t  frame.opcode
            //   std::vector<uint8_t> frame.params   // payload after the opcode
            //   std::vector<uint8_t> frame.data     // full payload incl. opcode
            //   bool     frame.is_broadcast
            ESP_LOGI("cec", "volume=%u", frame.params[0] & 0x7F);
```

Filters are evaluated in C++ before the automation fires — not in the generated
lambda.

### Action `hdmi_cec.transmit`

Two mutually exclusive shapes — structured (preferred) or raw (last resort):

```yaml
    # structured: opcode by name, params separate from the opcode
    - hdmi_cec.transmit:
        to: avr                     # name | number | broadcast; templatable
        from: tv                    # optional; send on behalf of another address
        opcode: user_control_pressed
        params: [0x41]              # optional; templatable

    # raw: full escape hatch, opcode + params as bytes
    - hdmi_cec.transmit:
        to: avr
        raw: [0x44, 0x41]           # templatable
```

Returns after the frame is acknowledged or the retransmit budget is exhausted.

---

## Layer 3 — semantic triggers & actions

Decoded, high-level events and commands. The user never touches an opcode.

### Triggers

```yaml
hdmi_cec:
  on_key_press:                  # User Control Pressed
    - then:
        - lambda: 'ESP_LOGI("cec", "key=%s from=%u", key.c_str(), source);'
        # exposed: std::string key (mapped name), uint8_t code, uint8_t source
  on_key_release:                # User Control Released

  on_standby:                    # a Standby was issued (exposed: uint8_t source)
  on_active_source:              # Active Source changed
                                 # exposed: uint16_t physical_address, uint8_t source
  on_volume:                     # Report Audio Status
                                 # exposed: uint8_t volume (0-100), bool mute, uint8_t source
  on_power:                      # Report Power Status of a tracked device
                                 # exposed: bool on, uint8_t source
```

### Actions

```yaml
    - hdmi_cec.power_on:   { device: tv }        # One Touch Play / Image View On
    - hdmi_cec.standby:    { device: tv }        # omit device → broadcast standby
    - hdmi_cec.active_source:                     # become the active source
    - hdmi_cec.volume:     { device: avr, action: up }   # up | down | mute
    - hdmi_cec.key_press:  { device: avr, key: volume_up }
                                                  # sends Pressed + Released with the
                                                  # correct inter-key delay, from the key map
```

### Key names

`key:` (in `on_key_press`/`on_key_release` and `hdmi_cec.key_press`) accepts any
name from the complete CEC User Control Code table below; the lambda also exposes
the raw `code`. A code outside the table is surfaced as its hex string
(`"0x5a"`), never dropped — coverage is total.

| Code | Name | Code | Name | Code | Name |
|------|------|------|------|------|------|
| 0x00 | `select` | 0x30 | `channel_up` | 0x60 | `play_function` |
| 0x01 | `up` | 0x31 | `channel_down` | 0x61 | `pause_play_function` |
| 0x02 | `down` | 0x32 | `previous_channel` | 0x62 | `record_function` |
| 0x03 | `left` | 0x33 | `sound_select` | 0x63 | `pause_record_function` |
| 0x04 | `right` | 0x34 | `input_select` | 0x64 | `stop_function` |
| 0x05 | `right_up` | 0x35 | `display_information` | 0x65 | `mute_function` |
| 0x06 | `right_down` | 0x36 | `help` | 0x66 | `restore_volume_function` |
| 0x07 | `left_up` | 0x37 | `page_up` | 0x67 | `tune_function` |
| 0x08 | `left_down` | 0x38 | `page_down` | 0x68 | `select_media_function` |
| 0x09 | `root_menu` | 0x40 | `power` | 0x69 | `select_av_input_function` |
| 0x0A | `setup_menu` | 0x41 | `volume_up` | 0x6A | `select_audio_input_function` |
| 0x0B | `contents_menu` | 0x42 | `volume_down` | 0x6B | `power_toggle_function` |
| 0x0C | `favorite_menu` | 0x43 | `mute` | 0x6C | `power_off_function` |
| 0x0D | `exit` | 0x44 | `play` | 0x6D | `power_on_function` |
| 0x10 | `media_top_menu` | 0x45 | `stop` | 0x71 | `f1_blue` |
| 0x11 | `media_context_menu` | 0x46 | `pause` | 0x72 | `f2_red` |
| 0x1D | `number_entry_mode` | 0x47 | `record` | 0x73 | `f3_green` |
| 0x1E | `number_11` | 0x48 | `rewind` | 0x74 | `f4_yellow` |
| 0x1F | `number_12` | 0x49 | `fast_forward` | 0x75 | `f5` |
| 0x20 | `number_0` | 0x4A | `eject` | 0x76 | `data` |
| 0x21–0x29 | `number_1`–`number_9` | 0x4B | `forward` | | |
| 0x2A | `dot` | 0x4C | `backward` | | |
| 0x2B | `enter` | 0x4D | `stop_record` | | |
| 0x2C | `clear` | 0x4E | `pause_record` | | |
| 0x2F | `next_favorite` | 0x50 | `angle` | | |
| | | 0x51 | `sub_picture` | | |
| | | 0x52 | `video_on_demand` | | |
| | | 0x53 | `electronic_program_guide` | | |
| | | 0x54 | `timer_programming` | | |
| | | 0x55 | `initial_configuration` | | |
| | | 0x56 | `select_broadcast_type` | | |
| | | 0x57 | `select_sound_presentation` | | |

Convenience aliases: `next` → `forward`, `previous` → `backward`, `back` →
`exit`.

---

## Layer 4 — entity platforms

The idiomatic ESPHome layer: real Home Assistant entities, no lambda. Each
platform reads/writes the **device-state registry** (see Architecture).

```yaml
switch:
  - platform: hdmi_cec
    hdmi_cec_id: bus
    name: "TV Power"
    device: tv                   # ON → power_on, OFF → standby
                                 # state tracked via Report Power Status

select:
  - platform: hdmi_cec
    name: "AVR Input"
    device: avr
    options:                     # label → physical address; selecting sends Set Stream Path
      "Apple TV": 0x4000
      "Blu-ray":  0x2000
    # Non-HDMI / proprietary inputs that don't answer Set Stream Path can instead
    # map a label to a raw command: { opcode: ..., params: [...] }.

sensor:
  - platform: hdmi_cec
    name: "AVR Volume"
    device: avr
    type: audio_volume           # 0-100 from Report Audio Status

binary_sensor:
  - platform: hdmi_cec
    name: "AVR Mute"
    device: avr
    type: mute                   # mute | power | active_source

text_sensor:
  - platform: hdmi_cec
    name: "TV Name"
    device: tv
    type: osd_name               # osd_name | power_state
```

---

## Architecture — the device-state registry

The entity platforms (§4) and the semantic triggers (§3) are only possible if the
component maintains a live model of the bus. This is the piece no existing ESPHome
CEC component has, and the reason this one can be "the best".

For every device named in `devices:`, the registry tracks:

- power state (on / standby / transitioning)
- audio volume + mute (for the audio system)
- active source (physical address currently routed)
- OSD name and vendor id, once learned

It is kept current two ways:

- **Passively**, by decoding every relevant frame that crosses the bus
  (`promiscuous_mode` is effectively required internally for tracked devices, even
  when user triggers only see addressed frames).
- **Actively**, by polling every `update_interval` and on demand (Give Device
  Power Status, Give Audio Status). Polling is a safety net, not the primary
  source: a poll is skipped if the bus carried a frame in the last ~200 ms, and
  never more than one active query is in flight, so the bus is never flooded.

Layers map onto the engine like this:

```
        raw bus frames
              │
     ┌────────▼─────────┐
     │  frame decoder   │──► Layer 2 on_frame (low-level, named)
     └────────┬─────────┘
              │ semantic parse
     ┌────────▼─────────┐
     │ device registry  │──► Layer 3 semantic triggers
     │  (per-device     │──► Layer 4 entity state (sensors read here)
     │   power/vol/src) │◄── Layer 4 entity commands (switch/select write here)
     └──────────────────┘         │
                                   ▼
                          Layer 0/2 transmitter
                       (arbitration + retransmission)
```

