# esphome-cec

An [ESPHome](https://esphome.io) external component that drives an **HDMI-CEC** bus using the ESP32 **RMT** peripheral.

> **Status: experimental.** It works and runs in production on an ESP32-S3, but the API is not yet idiomatic ESPHome (it requires C++) and several parts of the CEC protocol are unimplemented. See [ROADMAP.md](ROADMAP.md) and the limitations below.

## Why this component

The reference CEC component for ESPHome, [Palakis/esphome-native-hdmi-cec](https://github.com/Palakis/esphome-native-hdmi-cec), bit-bangs the bus from an ISR using busy-waits of roughly 1.5 ms. That has two consequences:

- **It does not work on the ESP32-S3** (or C3): it crashes at boot, with the added trap that OTA reports "successful" while the device has silently rolled back to the previous image. Only ESP32 classic, ESP8266 and RP2040 are supported.
- **It is hostile to real-time work**: holding a core for 1.5 ms under an interrupt lock breaks an audio pipeline.

This component hands timing to the hardware instead. Reception uses RMT capture with DMA, transmission uses the RMT encoder, and the ISR does nothing but push the capture onto a queue. The result runs on an ESP32-S3 that simultaneously decodes a 48 kHz FLAC stream to an SPDIF output, with no dropouts.

## Hardware

The CEC bus is a single open-drain wire, pulled high by the connected devices.

| HDMI pin | Signal | Connect to |
|---|---|---|
| 13 | CEC | any GPIO on the ESP32 |
| 17 | DDC/CEC ground | ESP32 GND |

A plain HDMI breakout board is enough. No additional components are required: the bus is already pulled high by the other devices, and the ESP32 only ever pulls it low.

**Tested on**: ESP32-S3 (N16R8), ESP-IDF 5.5, ESPHome 2026.7. Other ESP32 variants are unverified — see limitations.

## Usage

```yaml
external_components:
  - source:
      type: git
      url: https://github.com/mguaylam/esphome-cec
    components: [hdmi_cec_rmt]

hdmi_cec_rmt:
  id: cec_bus
  pin: 4
```

The API is currently **C++ only**, driven from a lambda. Receiving:

```yaml
esphome:
  on_boot:
    - lambda: |-
        id(cec_bus)->set_frame_handler(
          [](uint8_t initiator, uint8_t destination, const std::vector<uint8_t> &data) {
            if (data.empty()) return;
            uint8_t opcode = data[0];
            // Report Audio Status from an amplifier (logical address 5)
            if (initiator == 0x5 && opcode == 0x7A && data.size() >= 2) {
              bool mute = (data[1] & 0x80) != 0;
              uint8_t volume = data[1] & 0x7F;
              ESP_LOGI("cec", "volume=%u mute=%d", volume, mute);
            }
          });
```

Sending — `send(destination, payload)`, where the payload starts with the opcode:

```yaml
button:
  - platform: template
    name: "Amp volume up"
    on_press:
      - lambda: 'id(cec_bus)->send(5, {0x44, 0x41});'   # User Control Pressed
      - delay: 100ms
      - lambda: 'id(cec_bus)->send(5, {0x45});'          # User Control Released
```

`send_from(initiator, destination, payload)` sends on behalf of another logical address, which is useful for probing a device that only answers the TV.

The component's logical address is **hard-coded to `0x8`** (Playback 2) in `hdmi_cec_rmt.h`; making it configurable is task 2 on the roadmap.

## Diagnostics

`dump_config()` reports hardware state and cumulative counters:

```
HDMI-CEC RMT:
  Pin: GPIO4, logical address: 8
  RX channel: created, DMA: yes, last arm: OK (0)
  TX channel: ready, receiver ACK: active
  Raw captures: 13, frames decoded: 13, sent: 7, ACKs sent: 18, errors: 1
```

Every decoded frame is logged with its contents, addresses and acknowledgement result:

```
CEC frame [85:44:41] initiator=8 destination=5 eom=1 ack=yes
CEC frame [58:7A:33] initiator=5 destination=8 eom=1 ack=yes
```

## Known limitations

The component implements a minimum viable subset of the protocol. What is missing:

- **No collision arbitration.** CEC requires monitoring the line bit by bit while transmitting the header and backing off if another device is talking. This component only checks that the bus looks idle before it starts.
- **No retransmission.** The specification calls for up to five attempts when a frame is not acknowledged. Here, a lost frame stays lost.
- **No address negotiation.** The logical address is fixed at compile time, with no polling to confirm it is free. The physical address is not read from EDID, so the component cannot place itself correctly in the HDMI topology.
- **No Feature Abort.** Unhandled opcodes are silently ignored. That is deliberate — replying to everything floods the bus — but it is not compliant.
- **Portability unverified.** The receiver ACK manipulates GPIO registers directly (`GPIO.func_out_sel_cfg`). The code should work on other ESP32 variants, but only the S3 has been tested.
- **Occasional decode errors.** The counter sometimes shows a rejected frame on an otherwise quiet bus. The cause has not been established.

## A warning about real devices

CEC is a protocol many devices implement poorly. Observed while developing this component, on a **Yamaha RX-V375** (2013):

- Powering it on over CEC **reproducibly crashes its audio section**. It keeps answering the bus normally and shows the correct input signal, but no sound comes out, and only a mains power cycle recovers it. Powered on by hand, it works perfectly — including CEC volume control.
- The input-selection command (`User Control` `0x6A`) is acknowledged but has no effect.

Neither behaviour is caused by this component. They illustrate that CEC control must be validated device by device, and that an acknowledgement does not mean a command was carried out.

## License

MIT — see [LICENSE](LICENSE).
