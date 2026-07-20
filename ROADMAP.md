# Roadmap — bringing this component up to ESPHome standards

Ordered by dependency and payoff. Phase 1 is what stands between this repository and something another person can actually use; phase 2 is what stands between "works on my bus" and "correct". Later phases are polish and, optionally, upstreaming.

---

## Phase 1 — Make it usable without writing C++

Nothing else matters until this is done. Today a user must declare a 40-line lambda in `on_boot` and call `set_frame_handler()`, which is not how ESPHome components are consumed.

### 1.1 YAML trigger `on_message` — *the big one*

Expose a trigger with optional filtering, mirroring the Palakis API so users can migrate without rewriting their configuration:

```yaml
hdmi_cec_rmt:
  id: cec_bus
  pin: 4
  on_message:
    - opcode: 0x7A
      source: 0x5
      then:
        - lambda: 'ESP_LOGI("cec", "volume=%u", data[1] & 0x7F);'
    - then:
        - lambda: 'ESP_LOGD("cec", "any frame");'
```

Implementation notes:

- Add a `HdmiCecRmtTrigger : Trigger<uint8_t, uint8_t, std::vector<uint8_t>>` class exposing `source`, `destination` and `data` to the lambda.
- In `__init__.py`, build the schema with `automation.validate_automation` and register each trigger via `cg.register_component` + `await automation.build_automation(trig, [...], conf)`.
- Filters (`opcode`, `source`, `destination`) are evaluated in C++ before firing, not in the generated lambda.
- Keep `set_frame_handler()` as the internal mechanism; the trigger list becomes its first consumer.

### 1.2 YAML action `hdmi_cec_rmt.send`

```yaml
on_press:
  - hdmi_cec_rmt.send:
      destination: 0x5
      data: [0x44, 0x41]
```

Use templatable values (`cv.templatable`) so destination and payload can be computed at runtime.

### 1.3 Move configuration out of the header

`address_` is hard-coded to `0x8` in `hdmi_cec_rmt.h`. Move it — plus the fields that arrive with phase 2 — into the config schema:

| Option | Default | Notes |
|---|---|---|
| `address` | `0x8` | logical address |
| `physical_address` | none | needed for `Report Physical Address`; see 2.3 |
| `promiscuous_mode` | `false` | fire triggers for frames not addressed to us |
| `monitor_mode` | `false` | never drive the line (listen only) |

### 1.4 Ship a working example

`example/` should contain a config that compiles as-is and does something demonstrable — for instance, exposing an amplifier's volume as a sensor and volume up/down as buttons.

---

## Phase 2 — Protocol correctness

The component currently talks to a quiet bus with two or three devices and gets away with shortcuts. These are the shortcuts, in the order they will bite.

### 2.1 Collision arbitration

CEC is a multi-master bus. While transmitting the header block, a device must sample the line and abandon transmission if it reads back a level it did not drive — meaning a lower logical address won arbitration.

The current "is the bus idle?" check (GPIO level plus 10 ms of silence) does not detect a device that starts transmitting at the same instant. Both frames get corrupted.

The RX channel already listens to our own transmissions (`io_loop_back` is enabled on both channels), so the raw material is there. The work is comparing the captured header against what was sent, early enough to stop the transmission.

### 2.2 Retransmission on NACK

The specification allows up to five attempts for an unacknowledged frame. Acknowledgement is already detected via self-capture — it is logged as `ack=yes/no`. Feed that back into `send()`, with the mandated inter-attempt delay, and return a meaningful result to the caller.

### 2.3 Address handling

- **Logical address**: poll the chosen address at startup (send a zero-length frame to yourself; an acknowledgement means it is taken) and fall back to the next address of the same device type.
- **Physical address**: this is the one that matters most, and it is not solvable in software alone — it requires reading EDID from the HDMI connector, which a bare CEC-wire connection cannot do. Either accept a user-supplied value from the configuration and document its consequences honestly, or wire up DDC and read it. Note that advertising a plausible-looking but unnegotiated physical address is suspected of destabilising at least one amplifier, so the default should be to advertise nothing.

### 2.4 Minimal Feature Abort policy

Silence for everything is not compliant; replying to everything floods the bus and, historically, has crashed devices. The reasonable middle ground is to answer `Feature Abort` only for directly addressed messages that are neither handled nor broadcast, and never for broadcasts.

---

## Phase 3 — Portability

### 3.1 Other ESP32 variants

Only the S3 is tested. The receiver ACK writes GPIO registers directly:

```cpp
GPIO.func_out_sel_cfg[pin].val = out_sel_rmt_;
esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
```

Verify on ESP32 classic, C3, C6 and S2. If register layouts diverge, hide the difference behind the `hal/gpio_ll.h` abstraction rather than adding `#ifdef` blocks.

### 3.2 Declare the requirement in `__init__.py`

The component needs ESP-IDF (not Arduino) and an RMT peripheral with the new driver API. Validate that at config time with a clear error message, instead of failing with a compiler error.

---

## Phase 4 — Quality

### 4.1 Investigate the decode errors

`dump_config()` occasionally reports one rejected frame. Log the offending pulse widths and find out whether it is a real bus artefact, an off-by-one in the symbol capacity, or a race in the double-buffer handoff. Publishing a component while a known decoder anomaly is unexplained is not defensible.

### 4.2 Formatting and lint

ESPHome enforces `clang-format` (Google style, 120 columns) and `ruff`/`black` on Python. Add the config files and run both.

### 4.3 Component tests

ESPHome expects `tests/components/<name>/` with one YAML per target, verifying that the config compiles. Add `test-esp32-s3-idf.yaml` at minimum.

### 4.4 Continuous integration

A GitHub Actions workflow that compiles the test configurations on every push. This is what catches an ESPHome API change before a user does.

---

## Phase 5 — Optional: upstream to ESPHome

Only worth attempting once phases 1 through 4 are complete. It would require a documentation pull request against `esphome-docs`, a maintenance commitment via `CODEOWNERS`, and agreement with the maintainers on how it should coexist with the existing bit-banged CEC component — most likely as a second platform rather than a replacement.

---

## Where the code is

| File | Contents |
|---|---|
| `components/hdmi_cec_rmt/__init__.py` | config schema and code generation |
| `components/hdmi_cec_rmt/hdmi_cec_rmt.h` | class, constants, hardware state |
| `components/hdmi_cec_rmt/hdmi_cec_rmt.cpp` | RMT setup, receiver ACK, decoder, transmitter |

Three hard-won details are documented in the source comments and must survive any refactor: never call `rmt_receive()` from the driver's own ISR callback (it deadlocks the core); `signal_range_min_ns` cannot exceed about 3.2 µs; and DMA buffers on the S3 must be 64-byte aligned.
