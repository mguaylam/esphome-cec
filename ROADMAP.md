# Roadmap

The API is specified in [DESIGN.md](DESIGN.md); that document is the contract.
The full layered design is implemented — this file tracks what is done and what
remains.

## Done

- **Full YAML API, no C++ required.** Hub configuration (device type, named
  devices, OSD name, modes), the low-level `on_frame` trigger and
  `hdmi_cec.transmit` action with named opcodes, semantic triggers and actions
  (keys, standby, active source, volume, power), and the entity platforms
  (`sensor`, `binary_sensor`, `text_sensor`, `switch`, `select`).
- **Logical-address negotiation.** Probes the device type's address pool at
  startup and claims the first free one.
- **Retransmission on NACK.** Up to `retransmit` attempts, acknowledgement
  detected from the self-capture.
- **Standard-query responses (`auto_respond`).** Answers Give OSD Name, Give
  Device Power Status, Get CEC Version, Give Physical Address and Give Device
  Vendor ID, and Feature Aborts anything else directly addressed.
- **Device-state registry.** Tracks power, volume/mute, active source and OSD
  name per device, passively and by rate-limited polling.

## Remaining

### Protocol

- **Collision arbitration.** CEC is multi-master: while transmitting the header
  a device must sample the line and back off if it reads a level it did not
  drive. Today we only check that the bus looks idle before starting, and lean
  on retransmission to recover. The RX channel already hears our own
  transmissions (`io_loop_back`), so the raw material is there; the work is
  comparing the captured header against what was sent, early enough to stop.
- **Physical address discovery.** Reading it needs EDID over DDC, which a bare
  CEC-wire connection cannot do. Either wire up DDC and read it, or keep
  accepting a user-supplied value (current behaviour).

### Portability

- Only the ESP32-S3 is tested. Verify on ESP32 classic, C3, C6 and S2. The
  receiver ACK writes GPIO registers directly; if layouts diverge, hide the
  difference behind `hal/gpio_ll.h` rather than `#ifdef` blocks.

### Quality

- `clang-format` (Google style, 120 columns) and `ruff`/`black` config, run
  across the tree.
- Expand `tests/` and add a GitHub Actions workflow that compiles the test
  configuration on every push.
- Investigate the occasional decode error reported on a quiet bus.

### Upstream (optional)

- A documentation PR against `esphome-docs`, a `CODEOWNERS` maintenance
  commitment, and agreement with the maintainers. Would require a name distinct
  from the existing bit-banged CEC component to coexist.

## Where the code is

| File | Contents |
|---|---|
| `components/hdmi_cec/__init__.py` | hub schema, `on_frame`, `transmit`, semantic triggers/actions, entity helpers |
| `components/hdmi_cec/hdmi_cec.h` | component class, triggers, actions, registry, constants |
| `components/hdmi_cec/hdmi_cec.cpp` | RMT setup, receiver ACK, decoder, transmitter, negotiation, registry, housekeeping |
| `components/hdmi_cec/{sensor,binary_sensor,text_sensor,switch,select}/` | entity platforms |

Three hard-won details are documented in the source comments and must survive
any refactor: never call `rmt_receive()` from the driver's own ISR callback (it
deadlocks the core); `signal_range_min_ns` cannot exceed about 3.2 µs; and DMA
buffers on the S3 must be 64-byte aligned.
