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
- **Asynchronous transmission with collision arbitration.** The line is driven
  bit by bit from a gptimer ISR (never holding the core); the header is sampled
  for arbitration and yielded the instant a lower address wins, and the ACK slot
  is read inline. `send_from()` enqueues and returns immediately.
- **Retransmission on NACK.** Up to `retransmit` attempts, acknowledgement read
  from the ACK slot; collisions and NACKs retried with an address-weighted
  back-off.
- **Standard-query responses (`auto_respond`).** Answers Give OSD Name, Give
  Device Power Status, Get CEC Version, Give Physical Address and Give Device
  Vendor ID, and Feature Aborts anything else directly addressed.
- **Device-state registry.** Tracks power, volume/mute, active source and OSD
  name per device, passively and by rate-limited polling.

## Remaining

### Protocol

- **`physical_address: auto` via DDC/EDID.** The physical address is what an
  Active Source announcement needs to route audio to an amplifier — a bare
  CEC-wire connection cannot read it, so today it must be supplied by hand (and
  an invented value is the safe-but-imperfect fallback). The fix: wire the HDMI
  DDC lines (SCL pin 15, SDA pin 16, +5 V pin 18, ground) to the ESP32's I²C,
  read the sink's EDID (I²C address 0x50), parse its HDMI VSDB for the a.b.c.d
  physical address, and self-configure. This is the piece that makes audio
  routing reliable without guessing an address.

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
