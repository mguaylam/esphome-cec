# esphome-cec

[![CI](https://github.com/mguaylam/esphome-cec/actions/workflows/ci.yml/badge.svg)](https://github.com/mguaylam/esphome-cec/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/github/license/mguaylam/esphome-cec)](LICENSE)
[![ESPHome](https://img.shields.io/badge/ESPHome-external%20component-black?logo=esphome&logoColor=white)](https://esphome.io)

An [ESPHome](https://esphome.io) external component that drives an **HDMI-CEC** bus using the ESP32 **RMT** peripheral, with a full YAML API — triggers, actions and Home Assistant entities, no C++ required.

> **Status: experimental but functional.** Runs in production on an ESP32-S3. The CEC protocol is implemented end to end: address negotiation, receiver ACK, asynchronous transmission with collision arbitration, retransmission with context-aware signal free time, standard-query responses and a device-state model. Non-S3 targets are the main open item — see [ROADMAP.md](ROADMAP.md) and the limitations below.

## Why this component

The CEC bus has strict bit timing — a data bit lasts about 2.4 ms and must be sampled within a fraction of that. The usual way to meet it on an ESP is to bit-bang the line from an interrupt, busy-waiting through each bit for roughly 1.5 ms with a core held under an interrupt lock. That has two costs:

- **It breaks on modern ESP32s.** On the ESP32-S3 (and C3) a busy-wait of that length at boot crashes the device — with the added trap that OTA reports "successful" while the board has silently rolled back to the previous image.
- **It is hostile to real-time work.** Holding a core for 1.5 ms under an interrupt lock is enough to break an audio pipeline.

This component hands timing to the hardware instead. Reception uses RMT capture with DMA, and the ISR does nothing but push the capture onto a queue. Transmission is driven bit by bit from a hardware timer ISR that never holds the core, sampling the line inline for collision arbitration and the acknowledgement. The result runs on an ESP32-S3 that simultaneously decodes a 48 kHz FLAC stream to an SPDIF output, with no dropouts.

## Hardware

The CEC bus is a single open-drain wire (HDMI pin 13), pulled high by the
connected devices. The ESP32 only ever pulls it low, so **no extra components
are needed** — no resistor, no level shifter. Tap the wire on any spare HDMI
port of the TV or AV receiver: CEC is shared across all of a device's ports.

### Bill of materials

| Part | Why | ~Price | Where |
|---|---|---|---|
| ESP32-S3 DevKitC-1 (N16R8) | the MCU — RMT peripheral + PSRAM | ~$8 | [AliExpress](https://www.aliexpress.com/wholesale?SearchText=ESP32-S3-DevKitC-1+N16R8) |
| HDMI male breakout board (19-pin) | plugs into a spare HDMI port, exposes the pins | ~$2 | [AliExpress](https://www.aliexpress.com/wholesale?SearchText=HDMI+male+breakout+board+19+pin) |
| Female–female jumper wires | GPIO/GND → breakout | ~$1 | [AliExpress](https://www.aliexpress.com/wholesale?SearchText=dupont+female+jumper+wires) |

A female HDMI breakout plus a male-to-male cable works too, if you prefer an
inline tap. Any ESP32 with the new RMT driver should work, but only the S3 is
tested — see [Known limitations](#known-limitations).

### Pinout & wiring

Only two of the connector's pins matter for CEC:

| HDMI pin | Signal | ESP32 |
|---|---|---|
| 13 | CEC | any GPIO (e.g. GPIO4) |
| 17 | DDC/CEC ground | GND |

```
       ESP32-S3                          HDMI breakout plug
     ┌───────────┐                     ┌──────────────────┐
     │      GPIO4├─────────────────────┤ pin 13  CEC      │
     │       GND ├─────────────────────┤ pin 17  ground   │
     └───────────┘                     └────────┬─────────┘
                                                │
                                                ▼  any spare HDMI
                                                   port on TV / AVR
```

> Pins 15 (SCL), 16 (SDA) and 18 (+5 V) carry the DDC/EDID lines. They are only
> needed for the future `physical_address: auto` feature (see [ROADMAP.md](ROADMAP.md));
> a plain CEC hook-up leaves them unconnected.

**Tested on**: ESP32-S3 (N16R8), ESP-IDF 5.5, recent ESPHome.

## Step by step

From parts to a working CEC controller:

1. **Wire it.** ESP32 GPIO → HDMI pin 13, GND → pin 17 (see [Pinout & wiring](#pinout--wiring)),
   then plug the breakout into a spare HDMI port on the TV or AV receiver.

2. **Pull in the component.** Add it to your ESPHome YAML:

   ```yaml
   external_components:
     - source:
         type: git
         url: https://github.com/mguaylam/esphome-cec
       components: [hdmi_cec]
   ```

   It requires the **ESP-IDF** framework (not Arduino) and an ESP32 with the new
   RMT driver.

3. **Configure the bus.** A minimal hub:

   ```yaml
   esp32:
     board: esp32-s3-devkitc-1
     framework:
       type: esp-idf

   hdmi_cec:
     pin: GPIO4
     device_type: playback
     devices:
       tv: 0x0
       avr: 0x5
   ```

4. **Flash it.**

   ```bash
   esphome run your-config.yaml
   ```

5. **Verify.** The logs show the negotiated address and live bus traffic:

   ```
   CEC on GPIO4, address 8 (RX armed, TX ready, ACK active)
   CEC frame [45:44:41] initiator=4 destination=5 eom=1 ack=yes
   ```

6. **Control something.** Turn the AVR volume up:

   ```yaml
   button:
     - platform: template
       name: "AVR volume up"
       on_press:
         - hdmi_cec.volume: { device: avr, action: up }
   ```

The full API — hub options, entities, triggers and actions — follows.

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

- **Physical address is not auto-discovered.** Reading it needs EDID over DDC, which a bare CEC-wire connection cannot do. Supply it manually or leave it unset.
- **Portability unverified.** The receiver ACK manipulates GPIO registers directly (`GPIO.func_out_sel_cfg`). The code should work on other ESP32 variants, but only the S3 has been tested.
- **Rare spurious decode errors.** On a busy bus around 1-2 % of captures register an out-of-range pulse at a frame boundary (a symbol-merge artefact). The partial capture is discarded and reception re-syncs on the next frame — no valid frame is lost. The `dump_config` counter tracks them.

## A note on real devices

CEC is implemented unevenly across devices, and an acknowledgement on the bus means a frame was *received*, not that the command took effect. Validate control device by device.

## License

MIT — see [LICENSE](LICENSE).
