#include "hdmi_cec.h"

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

#include <driver/gpio.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <hal/gpio_ll.h>
#include <soc/gpio_sig_map.h>
#include <soc/gpio_struct.h>

#include <cstring>

namespace esphome {
namespace hdmi_cec {

static const char *const TAG = "hdmi_cec";

// ── CEC timings (µs) ──
// Reception: wide tolerances (the spec allows ±0.2/0.35 ms).
static constexpr uint32_t START_LOW_MIN = 3300;
static constexpr uint32_t START_LOW_MAX = 4100;
static constexpr uint32_t ZERO_LOW_MIN = 1200;
static constexpr uint32_t ZERO_LOW_MAX = 1900;
static constexpr uint32_t ONE_LOW_MIN = 350;
static constexpr uint32_t ONE_LOW_MAX = 950;
// Transmission: nominal values from the spec.
static constexpr uint32_t TX_START_LOW = 3700, TX_START_HIGH = 800;  // 4.5 ms total
static constexpr uint32_t TX_ZERO_LOW = 1500, TX_ZERO_HIGH = 900;    // 2.4 ms total
static constexpr uint32_t TX_ONE_LOW = 600, TX_ONE_HIGH = 1800;      // 2.4 ms total

// Logical-address pools per device type, in negotiation order. Indexed by the
// DeviceType enum value.
struct AddressPool {
  uint8_t addrs[4];
  uint8_t count;
};
static constexpr AddressPool ADDRESS_POOLS[] = {
    {{0x0}, 1},                 // DEVICE_TYPE_TV
    {{0x1, 0x2, 0x9}, 3},       // DEVICE_TYPE_RECORDER
    {{0x3, 0x6, 0x7, 0xA}, 4},  // DEVICE_TYPE_TUNER
    {{0x4, 0x8, 0xB}, 3},       // DEVICE_TYPE_PLAYBACK
    {{0x5}, 1},                 // DEVICE_TYPE_AUDIO_SYSTEM
    {{0xF}, 1},                 // DEVICE_TYPE_SWITCH (unregistered)
};

static const char *device_type_name(DeviceType type) {
  switch (type) {
    case DEVICE_TYPE_TV:
      return "tv";
    case DEVICE_TYPE_RECORDER:
      return "recorder";
    case DEVICE_TYPE_TUNER:
      return "tuner";
    case DEVICE_TYPE_PLAYBACK:
      return "playback";
    case DEVICE_TYPE_AUDIO_SYSTEM:
      return "audio_system";
    case DEVICE_TYPE_SWITCH:
      return "switch";
  }
  return "?";
}

// The CEC "Device Type" operand used in Report Physical Address. Note it is not
// the same numbering as our DeviceType enum (the CEC table reserves value 2).
static uint8_t cec_device_type_code(DeviceType type) {
  switch (type) {
    case DEVICE_TYPE_TV:
      return 0;
    case DEVICE_TYPE_RECORDER:
      return 1;
    case DEVICE_TYPE_TUNER:
      return 3;
    case DEVICE_TYPE_PLAYBACK:
      return 4;
    case DEVICE_TYPE_AUDIO_SYSTEM:
      return 5;
    case DEVICE_TYPE_SWITCH:
      return 6;
  }
  return 4;
}

// The complete CEC User Control Code table. Unmapped codes come back as their
// hex string so nothing is ever dropped.
static std::string key_name(uint8_t code) {
  switch (code) {
    case 0x00: return "select";
    case 0x01: return "up";
    case 0x02: return "down";
    case 0x03: return "left";
    case 0x04: return "right";
    case 0x05: return "right_up";
    case 0x06: return "right_down";
    case 0x07: return "left_up";
    case 0x08: return "left_down";
    case 0x09: return "root_menu";
    case 0x0A: return "setup_menu";
    case 0x0B: return "contents_menu";
    case 0x0C: return "favorite_menu";
    case 0x0D: return "exit";
    case 0x10: return "media_top_menu";
    case 0x11: return "media_context_menu";
    case 0x1D: return "number_entry_mode";
    case 0x1E: return "number_11";
    case 0x1F: return "number_12";
    case 0x20: return "number_0";
    case 0x21: return "number_1";
    case 0x22: return "number_2";
    case 0x23: return "number_3";
    case 0x24: return "number_4";
    case 0x25: return "number_5";
    case 0x26: return "number_6";
    case 0x27: return "number_7";
    case 0x28: return "number_8";
    case 0x29: return "number_9";
    case 0x2A: return "dot";
    case 0x2B: return "enter";
    case 0x2C: return "clear";
    case 0x2F: return "next_favorite";
    case 0x30: return "channel_up";
    case 0x31: return "channel_down";
    case 0x32: return "previous_channel";
    case 0x33: return "sound_select";
    case 0x34: return "input_select";
    case 0x35: return "display_information";
    case 0x36: return "help";
    case 0x37: return "page_up";
    case 0x38: return "page_down";
    case 0x40: return "power";
    case 0x41: return "volume_up";
    case 0x42: return "volume_down";
    case 0x43: return "mute";
    case 0x44: return "play";
    case 0x45: return "stop";
    case 0x46: return "pause";
    case 0x47: return "record";
    case 0x48: return "rewind";
    case 0x49: return "fast_forward";
    case 0x4A: return "eject";
    case 0x4B: return "forward";
    case 0x4C: return "backward";
    case 0x4D: return "stop_record";
    case 0x4E: return "pause_record";
    case 0x50: return "angle";
    case 0x51: return "sub_picture";
    case 0x52: return "video_on_demand";
    case 0x53: return "electronic_program_guide";
    case 0x54: return "timer_programming";
    case 0x55: return "initial_configuration";
    case 0x56: return "select_broadcast_type";
    case 0x57: return "select_sound_presentation";
    case 0x60: return "play_function";
    case 0x61: return "pause_play_function";
    case 0x62: return "record_function";
    case 0x63: return "pause_record_function";
    case 0x64: return "stop_function";
    case 0x65: return "mute_function";
    case 0x66: return "restore_volume_function";
    case 0x67: return "tune_function";
    case 0x68: return "select_media_function";
    case 0x69: return "select_av_input_function";
    case 0x6A: return "select_audio_input_function";
    case 0x6B: return "power_toggle_function";
    case 0x6C: return "power_off_function";
    case 0x6D: return "power_on_function";
    case 0x71: return "f1_blue";
    case 0x72: return "f2_red";
    case 0x73: return "f3_green";
    case 0x74: return "f4_yellow";
    case 0x75: return "f5";
    case 0x76: return "data";
    default: {
      char buf[6];
      snprintf(buf, sizeof(buf), "0x%02x", code);
      return buf;
    }
  }
}

// RMT driver "receive done" callback — ISR context.
static bool IRAM_ATTR rmt_rx_done_cb(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t *edata,
                                     void *user_ctx) {
  auto *self = static_cast<HdmiCec *>(user_ctx);
  return self->on_recv_done(edata);
}

bool IRAM_ATTR HdmiCec::on_recv_done(const rmt_rx_done_event_data_t *edata) {
  BaseType_t task_woken = pdFALSE;

  // FORBIDDEN here: any call into the RMT driver. rmt_receive() takes the
  // driver's internal lock; called from this callback (the same driver's ISR)
  // it spins forever and freezes the core — symptoms are a device that still
  // answers ping while its main loop is dead, with OTA and the API gone. The
  // ISR does nothing but enqueue; re-arming happens in the RX task.
  RxDoneEvent evt;
  evt.symbols = edata->received_symbols;
  evt.num_symbols = edata->num_symbols;
  this->raw_events_ = this->raw_events_ + 1;
  xQueueSendFromISR(this->rx_queue_, &evt, &task_woken);

  return task_woken == pdTRUE;
}

void HdmiCec::setup() {
  this->frame_queue_ = xQueueCreate(16, sizeof(DecodedFrame));
  this->rx_queue_ = xQueueCreate(8, sizeof(RxDoneEvent));
  if (this->rx_queue_ == nullptr) {
    ESP_LOGE(TAG, "Failed to create the RX queue");
    this->mark_failed();
    return;
  }

  rmt_rx_channel_config_t cfg = {};
  cfg.gpio_num = static_cast<gpio_num_t>(this->pin_);
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = 1 * 1000 * 1000;  // 1 µs per tick
  cfg.mem_block_symbols = 256;          // internal DMA buffer size
  cfg.flags.with_dma = true;            // S3: capture without a per-block interrupt
  cfg.flags.io_loop_back = true;        // pin shared with the TX channel (IDF 1-wire pattern)

  esp_err_t err = rmt_new_rx_channel(&cfg, &this->rx_channel_);
  this->dma_used_ = (err == ESP_OK);
  if (err != ESP_OK) {
    // Some chips or allocation states refuse DMA: retry without it.
    ESP_LOGW(TAG, "RMT DMA channel refused (%d), retrying without DMA", (int) err);
    cfg.flags.with_dma = false;
    cfg.mem_block_symbols = 64;
    err = rmt_new_rx_channel(&cfg, &this->rx_channel_);
  }
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "rmt_new_rx_channel failed: %d", (int) err);
    this->mark_failed();
    return;
  }

  rmt_rx_event_callbacks_t cbs = {};
  cbs.on_recv_done = rmt_rx_done_cb;
  err = rmt_rx_register_event_callbacks(this->rx_channel_, &cbs, this);
  if (err == ESP_OK)
    err = rmt_enable(this->rx_channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Could not enable the RMT channel: %d", (int) err);
    this->mark_failed();
    return;
  }

  this->arm_receive_(this->rx_buffers_[this->next_buffer_]);
  xTaskCreate(HdmiCec::rx_task_trampoline, "cec_rx", 4096, this, tskIDLE_PRIORITY + 3, &this->rx_task_handle_);
  this->setup_tx_();
  this->setup_ack_();

  // Resolve our logical address now that RX and TX are live.
  if (this->monitor_mode_) {
    this->address_ = ADDRESS_UNREGISTERED;
    ESP_LOGI(TAG, "Monitor mode: listen-only (address 0xF), never driving the bus");
  } else if (this->address_override_ != ADDRESS_AUTO) {
    this->address_ = this->address_override_;
    ESP_LOGI(TAG, "Using configured logical address %u (no negotiation)", (unsigned) this->address_);
  } else {
    this->negotiate_address_();
  }

  ESP_LOGI(TAG, "CEC on GPIO%u, address %u (RX %s, TX %s, ACK %s)", (unsigned) this->pin_, (unsigned) this->address_,
           this->last_arm_err_ == ESP_OK ? "armed" : "FAILED", this->tx_channel_ != nullptr ? "ready" : "MISSING",
           this->ack_timer_ != nullptr ? "active" : "MISSING");

  // Build the active-poll target list from the tracked devices.
  for (const auto &d : this->devices_) {
    this->poll_targets_.push_back({d.address, 0x8F});     // Give Device Power Status
    if (d.address == 0x05)                                // audio system
      this->poll_targets_.push_back({d.address, 0x71});  // Give Audio Status
  }
}

// ── Logical-address negotiation ─────────────────────────────────────────────
// Probe each address in the device type's pool by sending a header-only polling
// message; the first one nobody acknowledges is free, and we claim it. If the
// whole pool is taken (or TX is unavailable), fall back to unregistered (0xF),
// which is listen-only.

void HdmiCec::negotiate_address_() {
  const AddressPool &pool = ADDRESS_POOLS[this->device_type_];

  if (this->device_type_ == DEVICE_TYPE_SWITCH) {
    // A pure CEC switch has no address to claim.
    this->address_ = ADDRESS_UNREGISTERED;
    return;
  }
  if (this->tx_channel_ == nullptr) {
    this->address_ = pool.addrs[0];
    ESP_LOGW(TAG, "TX unavailable: cannot probe, defaulting to address %u without claiming it",
             (unsigned) this->address_);
    return;
  }

  for (uint8_t i = 0; i < pool.count; i++) {
    uint8_t candidate = pool.addrs[i];
    if (!this->poll_address_(candidate)) {
      this->address_ = candidate;
      this->negotiated_ = true;
      ESP_LOGI(TAG, "Claimed logical address %u", (unsigned) candidate);
      return;
    }
    ESP_LOGD(TAG, "Address %u already taken, trying next", (unsigned) candidate);
  }

  this->address_ = ADDRESS_UNREGISTERED;
  ESP_LOGW(TAG, "All %s addresses are taken: falling back to listen-only (0xF)",
           device_type_name(this->device_type_));
}

bool HdmiCec::poll_address_(uint8_t addr) {
  // The receiver-ACK ISR never fires while address_ is unregistered (see
  // gpio_edge_isr), so we cannot acknowledge our own poll: a "yes" here always
  // means another device owns the address.
  for (int attempt = 0; attempt < 3; attempt++) {
    this->probe_seen_ = false;
    this->probe_acked_ = false;
    this->probe_addr_ = static_cast<int8_t>(addr);

    bool sent = this->tx_frame_(addr, addr, {});  // header-only polling message
    if (sent) {
      // Wait for the RX task to decode our self-capture (it closes ~8 ms after
      // the frame ends, then is decoded). 80 ms is comfortable headroom.
      for (int i = 0; i < 16 && !this->probe_seen_; i++)
        vTaskDelay(pdMS_TO_TICKS(5));
      this->probe_addr_ = -1;
      if (this->probe_seen_)
        return this->probe_acked_;
    } else {
      this->probe_addr_ = -1;
      vTaskDelay(pdMS_TO_TICKS(20));  // bus busy: back off and retry
    }
  }
  // Never saw our own poll come back: inconclusive. Assume free rather than
  // stall the boot on a noisy bus.
  return false;
}

// ── Receiver ACK ──────────────────────────────────────────────────────────
// A GPIO ISR follows the bus edges and counts bits. On the falling edge of the
// ACK slot of a block addressed to us, the pin's output is switched from the
// RMT signal to "GPIO driving 0" (open drain → line pulled low, one register
// write), extending the initiator's "1" into a "0", which is the ACK. A gptimer
// releases the line 1.5 ms later by restoring the RMT routing. No busy-waiting,
// no driver calls from the ISR.

void IRAM_ATTR HdmiCec::gpio_edge_isr(void *arg) {
  auto *self = static_cast<HdmiCec *>(arg);
  int64_t now = esp_timer_get_time();
  gpio_num_t pin = static_cast<gpio_num_t>(self->pin_);

  if (gpio_ll_get_level(&GPIO, pin) == 0) {
    // Falling edge: a bit begins. Is this the ACK slot of a block for us?
    self->fall_us_ = now;
    if (self->in_frame_isr_ && self->frame_for_us_ && self->bit_idx_ == 9) {
      gpio_ll_set_level(&GPIO, pin, 0);
      esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
      gptimer_stop(self->ack_timer_);
      gptimer_set_raw_count(self->ack_timer_, 0);
      gptimer_start(self->ack_timer_);
      self->acks_sent_ = self->acks_sent_ + 1;
    }
    return;
  }

  // Rising edge: the bit value is given by how long the line stayed low.
  int64_t low = now - self->fall_us_;
  if (low >= 3300 && low <= 4100) {  // start bit
    self->in_frame_isr_ = true;
    self->first_block_ = true;
    self->frame_for_us_ = false;
    self->bit_idx_ = 0;
    self->cur_block_ = 0;
    return;
  }
  if (!self->in_frame_isr_)
    return;

  bool one = (low >= 350 && low <= 950);
  bool zero = (low >= 1200 && low <= 1900);
  if (!one && !zero) {
    self->in_frame_isr_ = false;  // out of spec: drop the frame
    return;
  }
  if (self->bit_idx_ < 9) {
    // Bits 0-7: data; bit 8: EOM.
    self->cur_block_ = (uint16_t) ((self->cur_block_ << 1) | (one ? 1 : 0));
    self->bit_idx_++;
    if (self->bit_idx_ == 9 && self->first_block_) {
      // Header complete: acknowledge only frames directed at our address, never
      // broadcasts (an ACK on a broadcast means "rejected") and never while
      // unregistered — 0xF is also the broadcast address, so it must not match.
      uint8_t dest = (uint8_t) ((self->cur_block_ >> 1) & 0x0F);
      self->frame_for_us_ = (dest == self->address_) && (self->address_ != ADDRESS_UNREGISTERED);
    }
  } else {
    // End of the ACK slot (including our own, read back ~1.5 ms) → next block.
    self->bit_idx_ = 0;
    self->cur_block_ = 0;
    self->first_block_ = false;
  }
}

bool IRAM_ATTR HdmiCec::ack_timer_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg) {
  auto *self = static_cast<HdmiCec *>(arg);
  // Release the line: restore the RMT routing (idle output = line released).
  GPIO.func_out_sel_cfg[self->pin_].val = self->out_sel_rmt_;
  gptimer_stop(timer);
  return false;
}

void HdmiCec::setup_ack_() {
  if (this->tx_channel_ == nullptr)
    return;  // the ACK reuses the open-drain pad configured by the TX channel

  gpio_num_t pin = static_cast<gpio_num_t>(this->pin_);
  // Remember the current output routing (the RMT TX signal) so it can be
  // restored, and preload the level register with 0: connecting SIG_GPIO_OUT
  // is then enough to pull the line low.
  this->out_sel_rmt_ = GPIO.func_out_sel_cfg[this->pin_].val;
  gpio_ll_set_level(&GPIO, pin, 0);

  gptimer_config_t tcfg = {};
  tcfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  tcfg.direction = GPTIMER_COUNT_UP;
  tcfg.resolution_hz = 1 * 1000 * 1000;
  esp_err_t err = gptimer_new_timer(&tcfg, &this->ack_timer_);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "gptimer unavailable (%d): receiver ACK disabled", (int) err);
    this->ack_timer_ = nullptr;
    return;
  }
  gptimer_event_callbacks_t tcbs = {};
  tcbs.on_alarm = ack_timer_cb;
  gptimer_register_event_callbacks(this->ack_timer_, &tcbs, this);
  gptimer_alarm_config_t acfg = {};
  // 1500 µs is the nominal duration of a CEC "0". An earlier value of 1300 µs
  // sat right on the lower tolerance edge (1.3-1.7 ms): too short an ACK can be
  // read back as a "1" by other devices and corrupt the bus.
  acfg.alarm_count = 1500;
  gptimer_set_alarm_action(this->ack_timer_, &acfg);
  gptimer_enable(this->ack_timer_);

  // Requires CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM=y, since the timer is started
  // and stopped from ISR context above.
  err = gpio_install_isr_service(0);
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {  // INVALID_STATE = already installed
    ESP_LOGW(TAG, "GPIO ISR service unavailable (%d): receiver ACK disabled", (int) err);
    return;
  }
  gpio_set_intr_type(pin, GPIO_INTR_ANYEDGE);
  gpio_isr_handler_add(pin, gpio_edge_isr, this);
  gpio_intr_enable(pin);
}

void HdmiCec::setup_tx_() {
  // Same GPIO as RX: open drain plus io_loop_back, following the 1-wire bus
  // pattern from the IDF examples. A "1" releases the line (the bus pull-up
  // takes over), a "0" pulls it to ground — we never fight other transmitters.
  rmt_tx_channel_config_t cfg = {};
  cfg.gpio_num = static_cast<gpio_num_t>(this->pin_);
  cfg.clk_src = RMT_CLK_SRC_DEFAULT;
  cfg.resolution_hz = 1 * 1000 * 1000;
  cfg.mem_block_symbols = 64;
  cfg.trans_queue_depth = 1;
  cfg.flags.io_od_mode = true;
  cfg.flags.io_loop_back = true;

  esp_err_t err = rmt_new_tx_channel(&cfg, &this->tx_channel_);
  if (err == ESP_OK) {
    rmt_copy_encoder_config_t enc_cfg = {};
    err = rmt_new_copy_encoder(&enc_cfg, &this->tx_encoder_);
  }
  if (err == ESP_OK)
    err = rmt_enable(this->tx_channel_);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "TX channel unavailable: %d (receive-only)", (int) err);
    this->tx_channel_ = nullptr;
    return;
  }

  // A freshly enabled TX channel may leave the line driven low: send a single
  // "released" symbol with eot_level=1 to free the bus.
  rmt_symbol_word_t release = {};
  release.level0 = 1;
  release.duration0 = 1;
  release.level1 = 1;
  release.duration1 = 1;
  rmt_transmit_config_t txc = {};
  txc.flags.eot_level = 1;
  rmt_transmit(this->tx_channel_, this->tx_encoder_, &release, sizeof(release), &txc);
  rmt_tx_wait_all_done(this->tx_channel_, 10);
}

bool HdmiCec::send_from(uint8_t initiator, uint8_t dest, const std::vector<uint8_t> &payload) {
  if (payload.empty()) {
    ESP_LOGW(TAG, "Empty payload rejected (a polling message is an internal operation, not send())");
    return false;
  }
  return this->tx_frame_(initiator, dest, payload);
}

bool HdmiCec::tx_frame_(uint8_t initiator, uint8_t dest, const std::vector<uint8_t> &payload) {
  if (this->tx_channel_ == nullptr) {
    ESP_LOGW(TAG, "TX unavailable");
    return false;
  }
  if (this->monitor_mode_) {
    ESP_LOGW(TAG, "Monitor mode: transmission refused");
    return false;
  }
  if (payload.size() > 14) {
    ESP_LOGW(TAG, "Invalid payload (%u bytes)", (unsigned) payload.size());
    return false;
  }

  // Is the bus free? Line high plus radio silence. A capture closes 8 ms after
  // the last frame, so >10 ms since then means >18 ms after the last bit; the
  // spec asks a new initiator to wait at least 5 bit periods, i.e. 12 ms.
  // NOTE: this is not arbitration. Two devices starting at the same instant
  // will still collide — see ROADMAP.md, task 2.1.
  if (gpio_get_level(static_cast<gpio_num_t>(this->pin_)) == 0 || (millis() - this->last_rx_ms_) < 10) {
    ESP_LOGW(TAG, "Bus busy, transmission cancelled (retry)");
    return false;
  }

  uint8_t blocks[15];
  size_t num_blocks = 0;
  blocks[num_blocks++] = (uint8_t) (((initiator & 0x0F) << 4) | (dest & 0x0F));
  for (uint8_t b : payload)
    blocks[num_blocks++] = b;

  size_t n = 0;
  this->tx_symbols_[n].level0 = 0;
  this->tx_symbols_[n].duration0 = TX_START_LOW;
  this->tx_symbols_[n].level1 = 1;
  this->tx_symbols_[n].duration1 = TX_START_HIGH;
  n++;
  for (size_t i = 0; i < num_blocks; i++) {
    bool eom = (i == num_blocks - 1);
    // 8 data bits (MSB first), then EOM, then the ACK slot sent as a "1" (line
    // released, so the addressed device can pull it low to acknowledge — which
    // shows up in the RX self-capture that follows).
    for (int bit = 9; bit >= 0; bit--) {
      bool one;
      if (bit >= 2)
        one = (blocks[i] >> (bit - 2)) & 1;
      else if (bit == 1)
        one = eom;
      else
        one = true;  // ACK slot
      this->tx_symbols_[n].level0 = 0;
      this->tx_symbols_[n].duration0 = one ? TX_ONE_LOW : TX_ZERO_LOW;
      this->tx_symbols_[n].level1 = 1;
      this->tx_symbols_[n].duration1 = one ? TX_ONE_HIGH : TX_ZERO_HIGH;
      n++;
    }
  }

  rmt_transmit_config_t txc = {};
  txc.flags.eot_level = 1;  // leave the line released when the frame ends
  esp_err_t err =
      rmt_transmit(this->tx_channel_, this->tx_encoder_, this->tx_symbols_, n * sizeof(rmt_symbol_word_t), &txc);
  if (err == ESP_OK)
    err = rmt_tx_wait_all_done(this->tx_channel_, 300);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "Transmission failed: %d", (int) err);
    return false;
  }
  this->frames_sent_++;
  ESP_LOGD(TAG, "Frame sent to %u (%u blocks)", (unsigned) dest, (unsigned) num_blocks);
  return true;
}

void HdmiCec::arm_receive_(rmt_symbol_word_t *buffer) {
  rmt_receive_config_t rx_cfg = {};
  // Glitch filter: counted on the SOURCE clock (80 MHz) in an 8-bit register,
  // so the maximum is about 3.2 µs. Asking for 50 µs fails with
  // ESP_ERR_INVALID_ARG (258) and the receiver is never armed — a silent
  // failure that is easy to misread as a wiring problem. 2 µs is plenty: CEC
  // pulses are hundreds of µs long.
  rx_cfg.signal_range_min_ns = 2 * 1000;
  rx_cfg.signal_range_max_ns = 8 * 1000 * 1000;
  esp_err_t err = rmt_receive(this->rx_channel_, buffer, RX_SYMBOL_CAPACITY * sizeof(rmt_symbol_word_t), &rx_cfg);
  this->last_arm_err_ = err;
  if (err != ESP_OK)
    ESP_LOGW(TAG, "rmt_receive failed: %d", (int) err);
}

void HdmiCec::loop() {
  // Dispatch decoded frames from the main loop, the only safe place to publish
  // to ESPHome entities and run automations.
  DecodedFrame f;
  for (int i = 0; i < 4 && this->frame_queue_ != nullptr && xQueueReceive(this->frame_queue_, &f, 0) == pdTRUE; i++) {
    if (f.len == 0)
      continue;

    Frame frame;
    frame.from = (uint8_t) (f.bytes[0] >> 4);
    frame.to = (uint8_t) (f.bytes[0] & 0x0F);
    frame.is_broadcast = (frame.to == 0x0F);
    frame.opcode = 0;
    if (f.len > 1) {
      frame.opcode = f.bytes[1];
      frame.data.assign(f.bytes + 1, f.bytes + f.len);
      if (f.len > 2)
        frame.params.assign(f.bytes + 2, f.bytes + f.len);
    }

    // Registry and semantic triggers snoop every frame, regardless of addressing.
    this->update_registry_(frame);
    this->process_semantic_(frame);

    // Legacy internal handler (set_frame_handler): sees every decoded frame,
    // with `data` being the payload after the header (including the opcode).
    if (this->frame_handler_)
      this->frame_handler_(frame.from, frame.to, frame.data);

    // on_frame triggers. Unless promiscuous, only frames addressed to us (or
    // broadcast) reach them.
    bool for_us = frame.is_broadcast || frame.to == this->address_;
    if (this->promiscuous_mode_ || for_us) {
      for (auto *trigger : this->frame_triggers_)
        trigger->process(frame);
    }

    // Answer standard queries on our own, if enabled.
    this->handle_housekeeping_(frame);
  }

  this->poll_registry_();
}

void HdmiCec::rx_task_trampoline(void *arg) { static_cast<HdmiCec *>(arg)->rx_task_(); }

void HdmiCec::rx_task_() {
  RxDoneEvent evt;
  while (true) {
    if (xQueueReceive(this->rx_queue_, &evt, portMAX_DELAY) != pdTRUE)
      continue;
    // Re-arm FIRST, on the other buffer (the capture we received owns its own),
    // so the receive window reopens as fast as possible; then decode.
    this->next_buffer_ ^= 1;
    this->arm_receive_(this->rx_buffers_[this->next_buffer_]);
    this->last_rx_ms_ = millis();
    this->decode_capture_(evt.symbols, evt.num_symbols);
  }
}

void HdmiCec::decode_capture_(const rmt_symbol_word_t *syms, size_t n) {
  uint8_t bytes[16];
  size_t num_bytes = 0;
  uint16_t block = 0;  // accumulator for the current 10-bit block
  uint8_t bit_count = 0;
  bool in_frame = false;
  bool last_eom = false;
  bool last_ack_low = false;

  auto flush_frame = [&]() {
    if (num_bytes == 0)
      return;
    char hex[3 * 16 + 1] = {0};
    for (size_t i = 0; i < num_bytes; i++)
      snprintf(hex + i * 3, 4, "%02X:", bytes[i]);
    hex[num_bytes * 3 - 1] = '\0';
    uint8_t initiator = bytes[0] >> 4;
    uint8_t dest = bytes[0] & 0x0F;
    // Directed frame: an ACK is the line being pulled low (bit read as "0").
    // On a broadcast the meaning is inverted — low means "rejected".
    bool acked = (dest == 0xF) ? !last_ack_low : last_ack_low;

    // Address negotiation: if this is the self-capture of our own polling
    // message (header only, initiator == dest == probed address), record
    // whether anybody acknowledged it and wake negotiate_address_().
    if (this->probe_addr_ >= 0 && num_bytes == 1 && initiator == (uint8_t) this->probe_addr_ &&
        dest == (uint8_t) this->probe_addr_) {
      this->probe_acked_ = acked;
      this->probe_seen_ = true;
    }

    ESP_LOGI(TAG, "CEC frame [%s] initiator=%u destination=%u eom=%d ack=%s", hex, initiator, dest, last_eom,
             acked ? "yes" : "no");
    this->frames_decoded_++;
    if (this->frame_queue_ != nullptr) {
      DecodedFrame f;
      f.len = (uint8_t) num_bytes;
      memcpy(f.bytes, bytes, num_bytes);
      xQueueSend(this->frame_queue_, &f, 0);  // queue full → frame dropped
    }
    num_bytes = 0;
  };

  for (size_t i = 0; i < n; i++) {
    // Each symbol is a low pulse (level0=0) followed by a high one.
    uint32_t low_us = (syms[i].level0 == 0) ? syms[i].duration0 : syms[i].duration1;

    if (low_us >= START_LOW_MIN && low_us <= START_LOW_MAX) {
      flush_frame();  // a previous frame was left without a clean ending
      in_frame = true;
      bit_count = 0;
      block = 0;
      continue;
    }
    if (!in_frame)
      continue;

    bool bit;
    if (low_us >= ONE_LOW_MIN && low_us <= ONE_LOW_MAX) {
      bit = true;
    } else if (low_us >= ZERO_LOW_MIN && low_us <= ZERO_LOW_MAX) {
      bit = false;
    } else {
      this->decode_errors_++;
      in_frame = false;
      flush_frame();
      continue;
    }

    block = (block << 1) | (bit ? 1 : 0);
    bit_count++;
    if (bit_count == 10) {
      if (num_bytes < sizeof(bytes))
        bytes[num_bytes++] = (block >> 2) & 0xFF;
      last_eom = (block & 0b10) != 0;
      last_ack_low = (block & 0b01) == 0;  // "0" = somebody pulled the line
      bit_count = 0;
      block = 0;
    }
  }
  flush_frame();
}

void HdmiCec::dump_config() {
  ESP_LOGCONFIG(TAG, "HDMI-CEC:");
  ESP_LOGCONFIG(TAG, "  Pin: GPIO%u", (unsigned) this->pin_);
  ESP_LOGCONFIG(TAG, "  Device type: %s", device_type_name(this->device_type_));
  if (this->monitor_mode_) {
    ESP_LOGCONFIG(TAG, "  Logical address: 0x%X (monitor mode, listen-only)", (unsigned) this->address_);
  } else {
    ESP_LOGCONFIG(TAG, "  Logical address: 0x%X (%s)", (unsigned) this->address_,
                  this->negotiated_ ? "negotiated" : "configured");
  }
  if (this->physical_address_ == PHYSICAL_ADDRESS_NONE) {
    ESP_LOGCONFIG(TAG, "  Physical address: none (advertising nothing)");
  } else {
    ESP_LOGCONFIG(TAG, "  Physical address: %u.%u.%u.%u", (unsigned) ((this->physical_address_ >> 12) & 0xF),
                  (unsigned) ((this->physical_address_ >> 8) & 0xF), (unsigned) ((this->physical_address_ >> 4) & 0xF),
                  (unsigned) (this->physical_address_ & 0xF));
  }
  ESP_LOGCONFIG(TAG, "  OSD name: '%s'", this->osd_name_.c_str());
  if (this->vendor_id_ != 0)
    ESP_LOGCONFIG(TAG, "  Vendor ID: 0x%06X", (unsigned) this->vendor_id_);
  ESP_LOGCONFIG(TAG, "  auto_respond: %s, promiscuous: %s, retransmit: %u, poll: %us",
                this->auto_respond_ ? "yes" : "no", this->promiscuous_mode_ ? "yes" : "no",
                (unsigned) this->retransmit_, (unsigned) (this->update_interval_ / 1000));
  if (!this->devices_.empty()) {
    ESP_LOGCONFIG(TAG, "  Tracked devices:");
    for (const auto &d : this->devices_) {
      const DeviceState &s = this->states_[d.address & 0x0F];
      const char *power = s.power == POWER_ON ? "on" : (s.power == POWER_STANDBY ? "standby" : "unknown");
      if (s.volume_known)
        ESP_LOGCONFIG(TAG, "    %s (0x%X): power=%s volume=%u mute=%d", d.name.c_str(), (unsigned) d.address, power,
                      (unsigned) s.volume, s.mute);
      else
        ESP_LOGCONFIG(TAG, "    %s (0x%X): power=%s", d.name.c_str(), (unsigned) d.address, power);
    }
  }
  ESP_LOGCONFIG(TAG, "  RX channel: %s, DMA: %s, last arm: %s (%d)",
                this->rx_channel_ != nullptr ? "created" : "MISSING", this->dma_used_ ? "yes" : "no",
                this->last_arm_err_ == ESP_OK ? "OK" : "ERROR", (int) this->last_arm_err_);
  ESP_LOGCONFIG(TAG, "  TX channel: %s, receiver ACK: %s", this->tx_channel_ != nullptr ? "ready" : "MISSING",
                this->ack_timer_ != nullptr ? "active" : "MISSING");
  ESP_LOGCONFIG(TAG, "  Raw captures: %u, frames decoded: %u, sent: %u, ACKs sent: %u, errors: %u",
                (unsigned) this->raw_events_, (unsigned) this->frames_decoded_, (unsigned) this->frames_sent_,
                (unsigned) this->acks_sent_, (unsigned) this->decode_errors_);
}

uint8_t HdmiCec::address_of(const std::string &name) const {
  for (const auto &d : this->devices_)
    if (d.name == name)
      return d.address;
  return 0xFF;
}

uint8_t HdmiCec::resolve_address(const std::string &token) const {
  if (token == "us")
    return this->address_;
  if (token == "broadcast")
    return 0x0F;
  uint8_t named = this->address_of(token);
  if (named != 0xFF)
    return named;
  ESP_LOGW(TAG, "Unknown address '%s', falling back to broadcast", token.c_str());
  return 0x0F;
}

// ── Device-state registry ───────────────────────────────────────────────────
// Passively keep each device's state current by decoding the reports that cross
// the bus. The registry snoops everything, even frames not addressed to us.

void HdmiCec::update_registry_(const Frame &frame) {
  DeviceState &st = this->states_[frame.from & 0x0F];
  switch (frame.opcode) {
    case 0x90:  // Report Power Status
      if (!frame.params.empty()) {
        // 0x00 = on, 0x01 = standby, 0x02 = transitioning to on, 0x03 = to standby.
        PowerState p = (frame.params[0] == 0x00 || frame.params[0] == 0x02) ? POWER_ON : POWER_STANDBY;
        if (st.power != p) {
          st.power = p;
          ESP_LOGD(TAG, "device 0x%X power=%s", (unsigned) frame.from, p == POWER_ON ? "on" : "standby");
        }
      }
      if (this->poll_in_flight_ && this->poll_wait_opcode_ == 0x90 && frame.from == this->poll_wait_addr_)
        this->poll_in_flight_ = false;
      break;
    case 0x7A:  // Report Audio Status
      if (!frame.params.empty()) {
        st.mute = (frame.params[0] & 0x80) != 0;
        st.volume = frame.params[0] & 0x7F;
        st.volume_known = true;
        ESP_LOGD(TAG, "device 0x%X volume=%u mute=%d", (unsigned) frame.from, (unsigned) st.volume, st.mute);
      }
      if (this->poll_in_flight_ && this->poll_wait_opcode_ == 0x7A && frame.from == this->poll_wait_addr_)
        this->poll_in_flight_ = false;
      break;
    case 0x82:  // Active Source (broadcast) — params are a physical address
      if (frame.params.size() >= 2) {
        st.active_source = (uint16_t) (frame.params[0] << 8) | frame.params[1];
        ESP_LOGD(TAG, "device 0x%X active source=%04X", (unsigned) frame.from, (unsigned) st.active_source);
      }
      break;
    case 0x47:  // Set OSD Name
      if (!frame.params.empty()) {
        st.osd_name.assign(frame.params.begin(), frame.params.end());
        ESP_LOGD(TAG, "device 0x%X osd_name='%s'", (unsigned) frame.from, st.osd_name.c_str());
      }
      break;
    default:
      break;
  }
}

// One rate-limited poll step per call: a safety net over the passive updates,
// never a flood. Round-robin over the tracked devices, one query in flight at a
// time, and never while the bus was busy in the last 200 ms.
void HdmiCec::poll_registry_() {
  if (this->poll_targets_.empty() || this->monitor_mode_ || this->tx_channel_ == nullptr)
    return;

  uint32_t now = millis();
  if (this->poll_in_flight_) {
    if (now - this->poll_sent_ms_ >= 1000)
      this->poll_in_flight_ = false;  // reply timed out
    else
      return;
  }
  if (now - this->last_poll_ms_ < this->update_interval_)
    return;
  if (now - this->last_rx_ms_ < 200)
    return;  // bus busy: back off

  const std::pair<uint8_t, uint8_t> &target = this->poll_targets_[this->poll_index_ % this->poll_targets_.size()];
  this->poll_index_++;
  this->last_poll_ms_ = now;

  if (this->send(target.first, {target.second})) {
    this->poll_in_flight_ = true;
    this->poll_sent_ms_ = now;
    this->poll_wait_addr_ = target.first;
    this->poll_wait_opcode_ = (target.second == 0x8F) ? 0x90 : 0x7A;  // expected reply
  }
}

// ── auto_respond housekeeping ────────────────────────────────────────────────
// Answer the standard queries every device is expected to handle, so the user
// never wires them by hand. Only for frames directly addressed to us (never a
// broadcast), never in monitor mode, never our own self-captures. Anything else
// directly addressed and unhandled gets a Feature Abort — the compliant middle
// ground between silence and flooding the bus.

void HdmiCec::feature_abort_(uint8_t initiator, uint8_t opcode) {
  // [Feature Abort][original opcode][reason: 0x00 = Unrecognized opcode].
  this->send(initiator, {0x00, opcode, 0x00});
}

void HdmiCec::handle_housekeeping_(const Frame &frame) {
  if (!this->auto_respond_ || this->monitor_mode_)
    return;
  if (frame.from == this->address_)
    return;  // our own transmission, seen via the self-capture
  if (frame.to != this->address_)
    return;  // only directly addressed — never broadcasts or other devices
  if (frame.data.empty())
    return;  // header-only polling message: the receiver ACK is the whole answer

  uint8_t initiator = frame.from;
  switch (frame.opcode) {
    case 0x46: {  // Give OSD Name → Set OSD Name
      std::vector<uint8_t> payload = {0x47};
      payload.insert(payload.end(), this->osd_name_.begin(), this->osd_name_.end());
      this->send(initiator, payload);
      break;
    }
    case 0x8F:  // Give Device Power Status → Report Power Status (always on)
      this->send(initiator, {0x90, 0x00});
      break;
    case 0x9F:  // Get CEC Version → CEC Version (0x05 = 1.4)
      this->send(initiator, {0x9E, 0x05});
      break;
    case 0x83:  // Give Physical Address → Report Physical Address (broadcast)
      if (this->physical_address_ != PHYSICAL_ADDRESS_NONE) {
        uint8_t hi = (this->physical_address_ >> 8) & 0xFF;
        uint8_t lo = this->physical_address_ & 0xFF;
        this->send_from(this->address_, 0x0F, {0x84, hi, lo, cec_device_type_code(this->device_type_)});
      } else {
        this->feature_abort_(initiator, frame.opcode);
      }
      break;
    case 0x8C:  // Give Device Vendor ID → Device Vendor ID (broadcast)
      if (this->vendor_id_ != 0) {
        this->send_from(this->address_, 0x0F,
                        {0x87, (uint8_t) (this->vendor_id_ >> 16), (uint8_t) (this->vendor_id_ >> 8),
                         (uint8_t) (this->vendor_id_)});
      } else {
        this->feature_abort_(initiator, frame.opcode);
      }
      break;
    default:  // directly addressed, unhandled, not a broadcast
      this->feature_abort_(initiator, frame.opcode);
      break;
  }
}

// ── Semantic (Layer 3) decode ────────────────────────────────────────────────
// Turn the opcodes that carry high-level meaning into typed trigger events.
// Observes every frame on the bus, like the registry.

void HdmiCec::process_semantic_(const Frame &frame) {
  switch (frame.opcode) {
    case 0x44:  // User Control Pressed
      if (!frame.params.empty()) {
        uint8_t code = frame.params[0];
        std::string name = key_name(code);
        for (auto *t : this->key_press_triggers_)
          t->trigger(name, code, frame.from);
      }
      break;
    case 0x45:  // User Control Released (carries no operand)
      for (auto *t : this->key_release_triggers_)
        t->trigger(std::string(), (uint8_t) 0, frame.from);
      break;
    case 0x36:  // Standby
      for (auto *t : this->standby_triggers_)
        t->trigger(frame.from);
      break;
    case 0x82:  // Active Source — params are a physical address
      if (frame.params.size() >= 2) {
        uint16_t pa = (uint16_t) (frame.params[0] << 8) | frame.params[1];
        for (auto *t : this->active_source_triggers_)
          t->trigger(pa, frame.from);
      }
      break;
    case 0x7A:  // Report Audio Status
      if (!frame.params.empty()) {
        uint8_t volume = frame.params[0] & 0x7F;
        bool mute = (frame.params[0] & 0x80) != 0;
        for (auto *t : this->volume_triggers_)
          t->trigger(volume, mute, frame.from);
      }
      break;
    case 0x90:  // Report Power Status
      if (!frame.params.empty()) {
        bool on = (frame.params[0] == 0x00 || frame.params[0] == 0x02);
        for (auto *t : this->power_triggers_)
          t->trigger(on, frame.from);
      }
      break;
    default:
      break;
  }
}

}  // namespace hdmi_cec
}  // namespace esphome
