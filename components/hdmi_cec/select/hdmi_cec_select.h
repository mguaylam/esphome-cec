#pragma once

#include "esphome/components/select/select.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include <string>
#include <utility>
#include <vector>

#include "../hdmi_cec.h"

namespace esphome {
namespace hdmi_cec {

// Input selector: each option maps a label to a physical address. Selecting one
// broadcasts Set Stream Path to route that address; the state follows the bus's
// current active source.
class HdmiCecSelect : public select::Select, public PollingComponent {
 public:
  void set_parent(HdmiCec *parent) { this->parent_ = parent; }
  void set_address(uint8_t address) { this->address_ = address; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void add_option(const std::string &label, uint16_t physical_address) {
    this->options_.push_back({label, physical_address});
  }

  void setup() override {
    if (this->address_ == 0xFF && !this->device_name_.empty()) {
      uint8_t resolved = this->parent_->address_of(this->device_name_);
      if (resolved != 0xFF)
        this->address_ = resolved;
      else
        ESP_LOGW("hdmi_cec.select", "unknown device '%s'", this->device_name_.c_str());
    }
  }

  void update() override {
    uint16_t active = this->parent_->current_active_source();
    if (active == PHYSICAL_ADDRESS_NONE)
      return;
    for (const auto &option : this->options_) {
      if (option.second == active) {
        if (!this->has_state() || this->state != option.first)
          this->publish_state(option.first);
        return;
      }
    }
  }

 protected:
  void control(const std::string &value) override {
    for (const auto &option : this->options_) {
      if (option.first == value) {
        uint16_t pa = option.second;
        // Set Stream Path (broadcast): ask the switch to route this address.
        this->parent_->send_from(this->parent_->address(), 0x0F,
                                 {0x86, (uint8_t) (pa >> 8), (uint8_t) (pa & 0xFF)});
        this->publish_state(value);  // optimistic; reconciled by update()
        return;
      }
    }
  }

  HdmiCec *parent_{nullptr};
  uint8_t address_{0xFF};
  std::string device_name_;
  std::vector<std::pair<std::string, uint16_t>> options_;
};

}  // namespace hdmi_cec
}  // namespace esphome
