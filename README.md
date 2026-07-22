# esphome-cec

An [ESPHome](https://esphome.io) external component that drives an **HDMI-CEC** bus using the ESP32 **RMT** peripheral, with a full YAML API — triggers, actions and Home Assistant entities, no C++ required.

> **Status: experimental but functional.** Runs in production on an ESP32-S3. Most of the CEC protocol is implemented (address negotiation, receiver ACK, retransmission, standard-query responses, a device-state model). Collision arbitration and non-S3 targets are still open — see [ROADMAP.md](ROADMAP.md) and the limitations below.

## Why this component

The CEC bus has strict bit timing — a data bit lasts about 2.4 ms and must be sampled within a fraction of that. The usual way to meet it on an ESP is to bit-bang the line from an interrupt, busy-waiting through each bit for roughly 1.5 ms with a core held under an interrupt lock. That has two costs:

- **It breaks on modern ESP32s.** On the ESP32-S3 (and C3) a busy-wait of that length at boot crashes the device — with the added trap that OTA reports "successful" while the board has silently rolled back to the previous image.
- **It is hostile to real-time work.** Holding a core for 1.5 ms under an interrupt lock is enough to break an audio pipeline.

This component hands timing to the hardware instead. Reception uses RMT capture with DMA, transmission uses the RMT encoder, and the ISR does nothing but push the capture onto a queue. The result runs on an ESP32-S3 that simultaneously decodes a 48 kHz FLAC stream to an SPDIF output, with no dropouts.

## Hardware

The CEC bus is a single open-drain wire, pulled high by the connected devices.

| HDMI pin | Signal | Connect to |
|---|---|---|
| 13 | CEC | any GPIO on the ESP32 |
| 17 | DDC/CEC ground | ESP32 GND |

A plain HDMI breakout board is enough. No additional components are required: the bus is already pulled high by the other devices, and the ESP32 only ever pulls it low.

**Tested on**: ESP32-S3 (N16R8), ESP-IDF 5.5, recent ESPHome. Other ESP32 variants are unverified — see limitations.

## Installation

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/mguaylam/esphome-cec
    components: [hdmi_cec]
```

The component requires the **ESP-IDF** framework (not Arduino) and an ESP32 with the new RMT driver.

## The hub

```yaml
hdmi_cec:
  id: cec_bus
  pin: GPIO4

  device_type: playback     # tv | recorder | tuner | playback | audio_system | switch
  osd_name: "ESPHome"       # advertised on Give OSD Name (<= 14 chars)
  physical_address: none    # none (default) | 0x1000 — see the note below
  vendor_id: 0x000000       # optional; advertised on Give Device Vendor ID

  auto_respond: true        # answer standard queries by itself (OSD name, power, version…)
  monitor_mode: false       # never drive the line, listen only
  promiscuous_mode: false   # fire on_frame for messages not addressed to us
  retransmit: 5             # attempts on a NACK (0 = fire and forget)
  update_interval: 30s      # how often the device-state registry actively polls

  # Give the addresses on your bus readable names, usable everywhere below.
  devices:
    tv: 0x0
    avr: 0x5
    apple_tv: 0x4
```

The logical address is **negotiated at startup**: the component probes its
`device_type`'s address pool and claims the first free one. Set `address:`
explicitly to skip negotiation.

`physical_address` cannot be read from a bare CEC wire (it needs EDID over DDC),
so it defaults to `none` — the component advertises nothing. Set it manually if
you know your device's position in the HDMI topology; an incorrect one can
confuse other devices, so `none` is the safe default.

## Entities

The platforms expose a tracked device's state to Home Assistant and let you
control it, with no lambdas.

```yaml
sensor:
  - platform: hdmi_cec
    name: "AVR volume"
    device: avr
    type: audio_volume        # 0-100

binary_sensor:
  - platform: hdmi_cec
    name: "AVR muted"
    device: avr
    type: mute                # mute | power

text_sensor:
  - platform: hdmi_cec
    name: "TV name"
    device: tv
    type: osd_name            # osd_name | power_state

switch:
  - platform: hdmi_cec
    name: "TV power"
    device: tv                # ON → One Touch Play, OFF → Standby

select:
  - platform: hdmi_cec
    name: "AVR input"
    device: avr
    options:                  # label → physical address; selecting routes it
      "Apple TV": 0x4000
      "Blu-ray":  0x2000
```

`device` accepts a name from `devices:` or a raw logical address.

## Automations

### Semantic triggers

React to decoded events without touching opcodes:

```yaml
hdmi_cec:
  on_volume:
    - then:
        - logger.log:
            format: "volume=%u mute=%d from %u"
            args: ["volume", "mute", "source"]
  on_key_press:
    - then:
        - logger.log:
            format: "key %s from %u"
            args: ["key.c_str()", "source"]
```

Available: `on_key_press`, `on_key_release`, `on_standby`, `on_active_source`,
`on_volume`, `on_power`.

### Semantic actions

```yaml
on_press:
  - hdmi_cec.power_on:  { device: tv }
  - hdmi_cec.standby:   { device: tv }       # omit device → broadcast
  - hdmi_cec.volume:    { device: avr, action: up }   # up | down | mute
  - hdmi_cec.key_press: { device: avr, key: play }    # any CEC user-control key
  - hdmi_cec.active_source: {}               # announce ourselves (needs physical_address)
```

### Low-level escape hatch

For anything the higher layers don't cover, the low-level layer speaks in named
opcodes and a structured `frame` object — never magic bytes, and the whole
protocol stays reachable.

```yaml
hdmi_cec:
  on_frame:
    - opcode: report_audio_status   # a name, or a raw byte like 0x7A
      from: avr                     # name | number | us | broadcast | any
      then:
        - logger.log:
            format: "audio status: 0x%02X"
            args: ["frame.params[0]"]

on_press:
  # Structured: named opcode + params, or a raw byte string.
  - hdmi_cec.transmit: { to: avr, opcode: user_control_pressed, params: [0x41] }
  - hdmi_cec.transmit: { to: broadcast, raw: [0x36] }
```

## Diagnostics

`dump_config()` reports the resolved configuration and cumulative counters:

```
HDMI-CEC:
  Pin: GPIO4
  Device type: playback
  Logical address: 0x4 (negotiated)
  Physical address: none (advertising nothing)
  OSD name: 'ESPHome'
  auto_respond: yes, promiscuous: no, retransmit: 5, poll: 30s
  Tracked devices:
    tv (0x0): power=on
    avr (0x5): power=on volume=42 mute=0
  RX channel: created, DMA: yes, last arm: OK (0)
  TX channel: ready, receiver ACK: active
  Raw captures: 41, frames decoded: 41, sent: 12, retransmits: 1, ACKs sent: 26, errors: 0
```

Every decoded frame is logged with its contents, addresses and acknowledgement result:

```
CEC frame [45:44:41] initiator=4 destination=5 eom=1 ack=yes
CEC frame [54:7A:33] initiator=5 destination=4 eom=1 ack=yes
```

## Known limitations

- **No collision arbitration.** CEC requires monitoring the line bit by bit while transmitting the header and backing off if another device is talking. This component only checks that the bus looks idle before it starts, then relies on retransmission to recover from a collision.
- **Physical address is not auto-discovered.** Reading it needs EDID over DDC, which a bare CEC-wire connection cannot do. Supply it manually or leave it unset.
- **Portability unverified.** The receiver ACK manipulates GPIO registers directly (`GPIO.func_out_sel_cfg`). The code should work on other ESP32 variants, but only the S3 has been tested.
- **Occasional decode errors.** The counter sometimes shows a rejected frame on an otherwise quiet bus. The cause has not been established.

## A note on real devices

CEC is implemented unevenly across devices, and an acknowledgement on the bus means a frame was *received*, not that the command took effect. Validate control device by device.

## License

MIT — see [LICENSE](LICENSE).
