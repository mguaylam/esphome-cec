#pragma once

#include <driver/gptimer.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "esphome/core/automation.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec {

// CEC device types. Each type owns a pool of logical addresses; at startup the
// component claims the first free one in the pool (see negotiate_address_).
enum DeviceType : uint8_t {
  DEVICE_TYPE_TV = 0,
  DEVICE_TYPE_RECORDER,
  DEVICE_TYPE_TUNER,
  DEVICE_TYPE_PLAYBACK,
  DEVICE_TYPE_AUDIO_SYSTEM,
  DEVICE_TYPE_SWITCH,
};

// Logical address 0xF: "unregistered" — listen only, never owns the bus. Also
// the broadcast destination, so it is never a valid address to acknowledge.
static constexpr uint8_t ADDRESS_UNREGISTERED = 0x0F;
// physical_address sentinel: nothing configured, advertise nothing.
static constexpr uint16_t PHYSICAL_ADDRESS_NONE = 0xFFFF;
// address override sentinel: no explicit address set → negotiate one.
static constexpr uint8_t ADDRESS_AUTO = 0xFF;

// A CEC frame is at most 16 blocks of 10 bits plus the start bit
// → ~161 low/high pairs. 192 symbols leaves comfortable headroom.
static constexpr size_t RX_SYMBOL_CAPACITY = 192;
static constexpr size_t TX_SEQ_CAPACITY = 1 + 16 * 10;  // start bit + 16 blocks × 10 bits

// Reference to a completed capture, posted from the RMT callback (ISR) to the
// RX task. The symbols stay in the buffer pointed to by `symbols` (double
// buffering: a buffer is never reused before it has been decoded).
struct RxDoneEvent {
  const rmt_symbol_word_t *symbols;
  size_t num_symbols;
};

// A fully decoded frame, handed from the RX task to loop() for dispatch to
// user code (ESPHome entities may only be touched from the main loop).
struct DecodedFrame {
  uint8_t len;
  uint8_t bytes[16];
};

// A logical address given a human-readable name in the configuration, so the
// rest of the config (and the higher layers) can refer to "avr" instead of 0x5.
struct NamedDevice {
  std::string name;
  uint8_t address;
};

enum PowerState : uint8_t {
  POWER_UNKNOWN = 0,
  POWER_ON,
  POWER_STANDBY,
};

// Live state tracked for a device on the bus, kept current by the registry
// (passive decode + active polling). Consumed by the higher layers.
struct DeviceState {
  PowerState power{POWER_UNKNOWN};
  uint8_t volume{0xFF};  // 0-100; only meaningful when volume_known
  bool mute{false};
  bool volume_known{false};
  uint16_t active_source{PHYSICAL_ADDRESS_NONE};  // physical address it announced
  std::string osd_name;
};

// A decoded CEC frame, exposed to `on_frame` lambdas as `frame`.
struct Frame {
  uint8_t from;                 // initiator logical address
  uint8_t to;                   // destination logical address (0xF = broadcast)
  uint8_t opcode;               // 0 for a header-only polling message (data empty)
  std::vector<uint8_t> params;  // payload after the opcode
  std::vector<uint8_t> data;    // full payload, including the opcode
  bool is_broadcast;
};

class HdmiCecFrameTrigger;
class KeyTrigger;
class StandbyTrigger;
class ActiveSourceTrigger;
class VolumeTrigger;
class PowerTrigger;

class HdmiCec : public Component {
 public:
  // ── Configuration (set from __init__.py) ─────────────────────────────────
  void set_pin(uint8_t pin) { this->pin_ = pin; }
  void set_device_type(DeviceType type) { this->device_type_ = type; }
  // Explicit logical address; skips negotiation (ADDRESS_AUTO = negotiate).
  void set_address_override(uint8_t address) { this->address_override_ = address; }
  void set_physical_address(uint16_t address) { this->physical_address_ = address; }
  void set_osd_name(const std::string &name) { this->osd_name_ = name; }
  void set_vendor_id(uint32_t vendor_id) { this->vendor_id_ = vendor_id; }
  void set_auto_respond(bool enabled) { this->auto_respond_ = enabled; }
  void set_monitor_mode(bool enabled) { this->monitor_mode_ = enabled; }
  void set_promiscuous_mode(bool enabled) { this->promiscuous_mode_ = enabled; }
  void set_retransmit(uint8_t attempts) { this->retransmit_ = attempts; }
  void set_update_interval(uint32_t interval_ms) { this->update_interval_ = interval_ms; }
  void add_device(const std::string &name, uint8_t address) { this->devices_.push_back({name, address}); }

  // Resolve a configured device name to its logical address, or 0xFF if unknown.
  // Consumed by the higher layers so their configs can speak in names.
  uint8_t address_of(const std::string &name) const;
  // The logical address actually in use (resolved in setup()).
  uint8_t address() const { return this->address_; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::HARDWARE; }

  // Sends a frame: header (our address → destination) plus payload (opcode and
  // arguments). Blocks for the duration of the frame (~50-80 ms). Returns false
  // if the bus is busy or TX is unavailable. The acknowledgement is visible in
  // the self-capture logged immediately afterwards.
  bool send(uint8_t dest, const std::vector<uint8_t> &payload) {
    return this->send_from(this->address_, dest, payload);
  }
  // Same, with an arbitrary initiator — lets you query a device "on behalf of
  // the TV": the reply goes to the real TV, which acknowledges it, and we
  // decode it passively in flight (no receiver ACK needed on our side).
  bool send_from(uint8_t initiator, uint8_t dest, const std::vector<uint8_t> &payload);

  // User handler: called from loop() for every decoded frame, with
  // (initiator, destination, data following the header).
  void set_frame_handler(std::function<void(uint8_t, uint8_t, const std::vector<uint8_t> &)> h) {
    this->frame_handler_ = std::move(h);
  }

  // Register an `on_frame` trigger (called from generated code).
  void register_frame_trigger(HdmiCecFrameTrigger *trigger) { this->frame_triggers_.push_back(trigger); }

  // Register the semantic (Layer 3) triggers (called from generated code).
  void register_key_trigger(KeyTrigger *trigger, bool release) {
    (release ? this->key_release_triggers_ : this->key_press_triggers_).push_back(trigger);
  }
  void register_standby_trigger(StandbyTrigger *trigger) { this->standby_triggers_.push_back(trigger); }
  void register_active_source_trigger(ActiveSourceTrigger *trigger) {
    this->active_source_triggers_.push_back(trigger);
  }
  void register_volume_trigger(VolumeTrigger *trigger) { this->volume_triggers_.push_back(trigger); }
  void register_power_trigger(PowerTrigger *trigger) { this->power_triggers_.push_back(trigger); }

  // The configured physical address, or PHYSICAL_ADDRESS_NONE. Used by the
  // active_source action.
  uint16_t physical_address() const { return this->physical_address_; }
  // Resolve a config address token — "us" (our negotiated address), "broadcast",
  // or a named device — to a logical address. Returns 0xF for an unknown token.
  uint8_t resolve_address(const std::string &token) const;

  // ── Device-state registry accessors (for the higher layers) ──────────────
  // Each comes in an address form and a device-name form.
  PowerState power_of(uint8_t addr) const { return this->states_[addr & 0x0F].power; }
  uint8_t volume_of(uint8_t addr) const {
    const DeviceState &s = this->states_[addr & 0x0F];
    return s.volume_known ? s.volume : 0xFF;
  }
  bool mute_of(uint8_t addr) const { return this->states_[addr & 0x0F].mute; }
  uint16_t active_source_of(uint8_t addr) const { return this->states_[addr & 0x0F].active_source; }
  std::string osd_name_of(uint8_t addr) const { return this->states_[addr & 0x0F].osd_name; }

  PowerState power_of(const std::string &name) const {
    uint8_t a = this->address_of(name);
    return a == 0xFF ? POWER_UNKNOWN : this->power_of(a);
  }
  uint8_t volume_of(const std::string &name) const {
    uint8_t a = this->address_of(name);
    return a == 0xFF ? 0xFF : this->volume_of(a);
  }
  bool mute_of(const std::string &name) const {
    uint8_t a = this->address_of(name);
    return a == 0xFF ? false : this->mute_of(a);
  }
  uint16_t active_source_of(const std::string &name) const {
    uint8_t a = this->address_of(name);
    return a == 0xFF ? PHYSICAL_ADDRESS_NONE : this->active_source_of(a);
  }
  std::string osd_name_of(const std::string &name) const {
    uint8_t a = this->address_of(name);
    return a == 0xFF ? std::string() : this->osd_name_of(a);
  }

  // Physical address of the current active source on the bus (the last device to
  // announce Active Source), or PHYSICAL_ADDRESS_NONE. Used by the select entity.
  uint16_t current_active_source() const { return this->current_active_source_; }

  // Called from the RMT driver's ISR callback.
  bool on_recv_done(const rmt_rx_done_event_data_t *edata);

 protected:
  void arm_receive_(rmt_symbol_word_t *buffer);
  void decode_capture_(const rmt_symbol_word_t *syms, size_t n);
  void setup_tx_();
  void setup_ack_();

  // Device-state registry: passive parse of one decoded frame, plus one
  // rate-limited active poll step (round-robin over the tracked devices).
  void update_registry_(const Frame &frame);
  void poll_registry_();

  // Decode one frame into the semantic (Layer 3) triggers.
  void process_semantic_(const Frame &frame);

  // auto_respond housekeeping: answer standard queries directed at us, and
  // Feature Abort anything else directly addressed and unhandled.
  void handle_housekeeping_(const Frame &frame);
  void feature_abort_(uint8_t initiator, uint8_t opcode);

  // Logical-address negotiation: probe the device type's pool and claim the
  // first free address. Falls back to unregistered (listen-only) if the whole
  // pool is taken or TX is unavailable.
  void negotiate_address_();
  // Send a header-only polling message to `addr` and return true if some device
  // acknowledged it — i.e. the address is already taken. Blocks briefly (boot only).
  bool poll_address_(uint8_t addr);

  // ── gptimer-driven asynchronous transmitter ──────────────────────────────
  // The line is driven bit by bit from a gptimer ISR (the receiver-ACK pattern):
  // the core is never held, arbitration is sampled during the header, and the
  // ACK slot is read inline. send_from() enqueues and returns; pump_tx_() (from
  // loop) starts the next frame when the bus is free and handles retransmission.
  bool enqueue_tx_(uint8_t initiator, uint8_t dest, const std::vector<uint8_t> &payload);
  void pump_tx_();
  void start_tx_current_(const uint8_t *blocks, uint8_t num_blocks);
  bool bus_free_() const;
  // Blocking transmit used only at boot (address negotiation), where the main
  // loop is not yet running. Returns whether the frame was acknowledged.
  bool transmit_sync_(uint8_t initiator, uint8_t dest, const std::vector<uint8_t> &payload, bool *acked);
  static bool tx_timer_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg);

  // Dedicated task: re-arms the capture (<1 ms) and decodes. loop() was too
  // slow (period up to 16 ms): the single reply from a follower we acknowledged
  // arrives ~17 ms after our frame and kept landing in the re-arm blind spot.
  static void rx_task_trampoline(void *arg);
  void rx_task_();
  TaskHandle_t rx_task_handle_{nullptr};

  // ISR: bit-level tracking of the bus plus assertion of the receiver ACK.
  static void gpio_edge_isr(void *arg);
  static bool ack_timer_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg);

  // ── Configuration ──
  DeviceType device_type_{DEVICE_TYPE_PLAYBACK};
  uint8_t address_override_{ADDRESS_AUTO};
  uint16_t physical_address_{PHYSICAL_ADDRESS_NONE};
  std::string osd_name_{"ESPHome"};
  uint32_t vendor_id_{0};
  bool auto_respond_{true};
  bool monitor_mode_{false};
  bool promiscuous_mode_{false};
  uint8_t retransmit_{5};
  uint32_t update_interval_{30000};
  std::vector<NamedDevice> devices_;

  uint8_t pin_{4};
  uint8_t address_{ADDRESS_UNREGISTERED};  // resolved in setup() (negotiation or override)
  rmt_channel_handle_t rx_channel_{nullptr};
  rmt_channel_handle_t tx_channel_{nullptr};
  rmt_encoder_handle_t tx_encoder_{nullptr};
  QueueHandle_t rx_queue_{nullptr};
  QueueHandle_t frame_queue_{nullptr};
  std::function<void(uint8_t, uint8_t, const std::vector<uint8_t> &)> frame_handler_{};
  std::vector<HdmiCecFrameTrigger *> frame_triggers_;
  std::vector<KeyTrigger *> key_press_triggers_;
  std::vector<KeyTrigger *> key_release_triggers_;
  std::vector<StandbyTrigger *> standby_triggers_;
  std::vector<ActiveSourceTrigger *> active_source_triggers_;
  std::vector<VolumeTrigger *> volume_triggers_;
  std::vector<PowerTrigger *> power_triggers_;

  // ── Device-state registry ──
  DeviceState states_[16];                                 // indexed by logical address
  uint16_t current_active_source_{PHYSICAL_ADDRESS_NONE};  // last announced active source
  std::vector<std::pair<uint8_t, uint8_t>> poll_targets_;  // (address, query opcode)
  size_t poll_index_{0};
  uint32_t last_poll_ms_{0};
  bool poll_in_flight_{false};
  uint32_t poll_sent_ms_{0};
  uint8_t poll_wait_addr_{0};
  uint8_t poll_wait_opcode_{0};

  // Double buffer: the RX task re-arms reception on the other buffer while it
  // decodes the one that just filled up.
  // alignas(64): DMA on the S3 requires cache-line aligned buffers (64 bytes);
  // 192 symbols × 4 bytes = 768 bytes, a multiple of 64. ✓
  alignas(64) rmt_symbol_word_t rx_buffers_[2][RX_SYMBOL_CAPACITY];
  volatile uint8_t next_buffer_{0};

  // ── gptimer transmitter state ──
  // One CEC bit driven as a low pulse then a release, both bit-dependent.
  struct TxBit {
    uint16_t low_us;
    uint16_t high_us;
    bool arbitrate;  // header "1" bit: yield if the line reads back low
    bool ack_slot;   // read the follower's acknowledgement here
  };
  struct TxJob {
    uint8_t blocks[16];
    uint8_t num_blocks;
    uint8_t attempts;
  };
  gptimer_handle_t tx_timer_{nullptr};
  TxJob tx_queue_[8];
  uint8_t tx_q_head_{0};
  uint8_t tx_q_tail_{0};
  uint32_t tx_backoff_until_{0};

  TxBit tx_seq_[TX_SEQ_CAPACITY];  // bit sequence of the frame currently on the wire
  volatile uint16_t tx_len_{0};
  volatile uint16_t tx_idx_{0};
  volatile uint8_t tx_phase_{0};
  volatile bool tx_active_{false};
  volatile bool tx_done_{true};
  volatile bool tx_arb_lost_{false};
  volatile bool tx_ack_low_{false};
  bool tx_in_flight_{false};  // a job at the queue head is on the wire

  // Diagnostic counters
  uint32_t frames_decoded_{0};
  uint32_t decode_errors_{0};
  volatile uint32_t raw_events_{0};  // completed RMT captures (incremented in the ISR)
  uint32_t frames_sent_{0};
  uint32_t retransmits_{0};  // frames re-sent after a NACK
  bool dma_used_{false};
  bool negotiated_{false};  // true if address_ came from negotiation, not an override
  esp_err_t last_arm_err_{ESP_OK};
  uint32_t last_rx_ms_{0};  // last bus activity seen by the RX task

  // ── Receiver ACK — state touched from the ISR only ──
  gptimer_handle_t ack_timer_{nullptr};
  uint32_t out_sel_rmt_{0};      // RMT output routing, restored after the ACK
  volatile int64_t fall_us_{0};  // timestamp of the last falling edge
  volatile bool in_frame_isr_{false};
  volatile bool first_block_{false};
  volatile bool frame_for_us_{false};
  volatile uint8_t bit_idx_{0};  // 0-8 = data + EOM received, 9 = ACK slot next
  volatile uint16_t cur_block_{0};
  volatile uint32_t acks_sent_{0};
};

// on_frame: fires for every decoded frame that passes the C++ filters. Filter
// values follow this convention: -1 = any (no filter), -2 = "us" (our own
// address, resolved at runtime because it is negotiated), 0..15 = a literal
// logical address (0xF = broadcast).
class HdmiCecFrameTrigger : public Trigger<Frame> {
 public:
  explicit HdmiCecFrameTrigger(HdmiCec *parent) : parent_(parent) { parent->register_frame_trigger(this); }
  void set_opcode(int16_t opcode) { this->opcode_ = opcode; }
  void set_from(int16_t addr) { this->from_ = addr; }
  void set_to(int16_t addr) { this->to_ = addr; }

  void process(const Frame &frame) {
    if (this->opcode_ >= 0 && (frame.data.empty() || frame.opcode != (uint8_t)this->opcode_)) return;
    if (this->from_ != -1) {
      uint8_t want = (this->from_ == -2) ? this->parent_->address() : (uint8_t)this->from_;
      if (frame.from != want) return;
    }
    if (this->to_ != -1) {
      uint8_t want = (this->to_ == -2) ? this->parent_->address() : (uint8_t)this->to_;
      if (frame.to != want) return;
    }
    this->trigger(frame);
  }

 protected:
  HdmiCec *parent_;
  int16_t opcode_{-1};
  int16_t from_{-1};
  int16_t to_{-1};
};

// hdmi_cec.transmit: structured (opcode [+ params]) or raw. Addresses given as a
// name/"us"/"broadcast" are resolved at runtime via a token; numbers and lambdas
// go through the templatable value.
template <typename... Ts>
class TransmitAction : public Action<Ts...> {
 public:
  explicit TransmitAction(HdmiCec *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, to)
  TEMPLATABLE_VALUE(uint8_t, source)
  TEMPLATABLE_VALUE(uint8_t, opcode)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, params)
  TEMPLATABLE_VALUE(std::vector<uint8_t>, raw)

  void set_to_token(const std::string &token) {
    this->to_token_ = token;
    this->has_to_token_ = true;
  }
  void set_from_token(const std::string &token) {
    this->from_token_ = token;
    this->has_from_token_ = true;
  }
  void set_has_source(bool value) { this->has_source_ = value; }
  void set_has_opcode(bool value) { this->has_opcode_ = value; }
  void set_has_params(bool value) { this->has_params_ = value; }
  void set_has_raw(bool value) { this->has_raw_ = value; }

  void play(Ts... x) override {
    uint8_t to = this->has_to_token_ ? this->parent_->resolve_address(this->to_token_) : this->to_.value(x...);
    uint8_t from;
    if (this->has_from_token_)
      from = this->parent_->resolve_address(this->from_token_);
    else if (this->has_source_)
      from = this->source_.value(x...);
    else
      from = this->parent_->address();

    std::vector<uint8_t> payload;
    if (this->has_raw_) {
      payload = this->raw_.value(x...);
    } else {
      payload.push_back(this->opcode_.value(x...));
      if (this->has_params_) {
        std::vector<uint8_t> p = this->params_.value(x...);
        payload.insert(payload.end(), p.begin(), p.end());
      }
    }
    this->parent_->send_from(from, to, payload);
  }

 protected:
  HdmiCec *parent_;
  std::string to_token_;
  std::string from_token_;
  bool has_to_token_{false};
  bool has_from_token_{false};
  bool has_source_{false};
  bool has_opcode_{false};
  bool has_params_{false};
  bool has_raw_{false};
};

// ── Semantic (Layer 3) triggers ──────────────────────────────────────────────
// One class is shared by on_key_press and on_key_release; the constructor flag
// routes it into the right list.
class KeyTrigger : public Trigger<std::string, uint8_t, uint8_t> {
 public:
  KeyTrigger(HdmiCec *parent, bool release) { parent->register_key_trigger(this, release); }
};
class StandbyTrigger : public Trigger<uint8_t> {
 public:
  explicit StandbyTrigger(HdmiCec *parent) { parent->register_standby_trigger(this); }
};
class ActiveSourceTrigger : public Trigger<uint16_t, uint8_t> {
 public:
  explicit ActiveSourceTrigger(HdmiCec *parent) { parent->register_active_source_trigger(this); }
};
class VolumeTrigger : public Trigger<uint8_t, bool, uint8_t> {
 public:
  explicit VolumeTrigger(HdmiCec *parent) { parent->register_volume_trigger(this); }
};
class PowerTrigger : public Trigger<bool, uint8_t> {
 public:
  explicit PowerTrigger(HdmiCec *parent) { parent->register_power_trigger(this); }
};

// ── Semantic (Layer 3) actions ───────────────────────────────────────────────
// Resolves a device destination the same way TransmitAction does: a name/keyword
// token, a templatable value, or (when neither is set) the broadcast address.
template <typename... Ts>
class SemanticActionBase : public Action<Ts...> {
 public:
  explicit SemanticActionBase(HdmiCec *parent) : parent_(parent) {}
  TEMPLATABLE_VALUE(uint8_t, device)
  void set_device_token(const std::string &token) {
    this->device_token_ = token;
    this->has_device_token_ = true;
  }
  void set_has_device(bool value) { this->has_device_value_ = value; }

 protected:
  uint8_t resolve_device_(Ts... x) {
    if (this->has_device_token_) return this->parent_->resolve_address(this->device_token_);
    if (this->has_device_value_) return this->device_.value(x...);
    return 0x0F;  // broadcast
  }
  HdmiCec *parent_;
  std::string device_token_;
  bool has_device_token_{false};
  bool has_device_value_{false};
};

// power_on (Image View On) and standby (Standby): a fixed opcode to a device.
template <typename... Ts>
class OpcodeAction : public SemanticActionBase<Ts...> {
 public:
  OpcodeAction(HdmiCec *parent, uint8_t opcode) : SemanticActionBase<Ts...>(parent), opcode_(opcode) {}
  void play(Ts... x) override { this->parent_->send(this->resolve_device_(x...), {this->opcode_}); }

 protected:
  uint8_t opcode_;
};

// key_press and volume: User Control Pressed(code) then Released to a device.
template <typename... Ts>
class KeyPressAction : public SemanticActionBase<Ts...> {
 public:
  explicit KeyPressAction(HdmiCec *parent) : SemanticActionBase<Ts...>(parent) {}
  TEMPLATABLE_VALUE(uint8_t, key)
  void play(Ts... x) override {
    uint8_t dest = this->resolve_device_(x...);
    uint8_t code = this->key_.value(x...);
    this->parent_->send(dest, {0x44, code});  // User Control Pressed
    this->parent_->send(dest, {0x45});        // User Control Released
  }
};

// active_source: broadcast Active Source with our configured physical address.
template <typename... Ts>
class ActiveSourceAction : public Action<Ts...> {
 public:
  explicit ActiveSourceAction(HdmiCec *parent) : parent_(parent) {}
  void play(Ts... x) override {
    uint16_t pa = this->parent_->physical_address();
    if (pa == PHYSICAL_ADDRESS_NONE) {
      ESP_LOGW("hdmi_cec", "active_source: no physical_address configured, nothing to announce");
      return;
    }
    this->parent_->send_from(this->parent_->address(), 0x0F, {0x82, (uint8_t)(pa >> 8), (uint8_t)(pa & 0xFF)});
  }

 protected:
  HdmiCec *parent_;
};

}  // namespace hdmi_cec
}  // namespace esphome
