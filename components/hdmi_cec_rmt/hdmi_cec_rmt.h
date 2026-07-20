#pragma once

#include "esphome/core/component.h"

#include <driver/gptimer.h>
#include <driver/rmt_rx.h>
#include <driver/rmt_tx.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include <functional>
#include <vector>

namespace esphome {
namespace hdmi_cec_rmt {

// A CEC frame is at most 16 blocks of 10 bits plus the start bit
// → ~161 low/high pairs. 192 symbols leaves comfortable headroom.
static constexpr size_t RX_SYMBOL_CAPACITY = 192;
static constexpr size_t TX_SYMBOL_CAPACITY = 176;  // start + 16 blocks × 10 bits

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

class HdmiCecRmt : public Component {
 public:
  void set_pin(uint8_t pin) { this->pin_ = pin; }

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

  // Called from the RMT driver's ISR callback.
  bool on_recv_done(const rmt_rx_done_event_data_t *edata);

 protected:
  void arm_receive_(rmt_symbol_word_t *buffer);
  void decode_capture_(const rmt_symbol_word_t *syms, size_t n);
  void setup_tx_();
  void setup_ack_();

  // Dedicated task: re-arms the capture (<1 ms) and decodes. loop() was too
  // slow (period up to 16 ms): the single reply from a follower we acknowledged
  // arrives ~17 ms after our frame and kept landing in the re-arm blind spot.
  static void rx_task_trampoline(void *arg);
  void rx_task_();
  TaskHandle_t rx_task_handle_{nullptr};

  // ISR: bit-level tracking of the bus plus assertion of the receiver ACK.
  static void gpio_edge_isr(void *arg);
  static bool ack_timer_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *arg);

  uint8_t pin_{4};
  uint8_t address_{0x8};  // Playback 2 — assumed free, never actually claimed (see roadmap 2.3)
  rmt_channel_handle_t rx_channel_{nullptr};
  rmt_channel_handle_t tx_channel_{nullptr};
  rmt_encoder_handle_t tx_encoder_{nullptr};
  QueueHandle_t rx_queue_{nullptr};
  QueueHandle_t frame_queue_{nullptr};
  std::function<void(uint8_t, uint8_t, const std::vector<uint8_t> &)> frame_handler_{};

  // Double buffer: the RX task re-arms reception on the other buffer while it
  // decodes the one that just filled up.
  // alignas(64): DMA on the S3 requires cache-line aligned buffers (64 bytes);
  // 192 symbols × 4 bytes = 768 bytes, a multiple of 64. ✓
  alignas(64) rmt_symbol_word_t rx_buffers_[2][RX_SYMBOL_CAPACITY];
  volatile uint8_t next_buffer_{0};

  rmt_symbol_word_t tx_symbols_[TX_SYMBOL_CAPACITY];

  // Diagnostic counters
  uint32_t frames_decoded_{0};
  uint32_t decode_errors_{0};
  volatile uint32_t raw_events_{0};  // completed RMT captures (incremented in the ISR)
  uint32_t frames_sent_{0};
  bool dma_used_{false};
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

}  // namespace hdmi_cec_rmt
}  // namespace esphome
