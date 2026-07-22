#pragma once

#include "esphome/components/switch/switch.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include <string>

#include "../hdmi_cec.h"

namespace esphome {
namespace hdmi_cec {

// Power switch for a tracked device: ON = One Touch Play (Image View On),
// OFF = Standby. State mirrors the registry's power state.
class HdmiCecSwitch : public switch_::Switch, public PollingComponent {
 public:
  void set_parent(HdmiCec *parent) { this->parent_ = parent; }
  void set_address(uint8_t address) { this->address_ = address; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }

  void setup() override {
    if (this->address_ == 0xFF && !this->device_name_.empty()) {
      uint8_t resolved = this->parent_->address_of(this->device_name_);
      if (resolved != 0xFF)
        this->address_ = resolved;
      else
        ESP_LOGW("hdmi_cec.switch", "unknown device '%s'", this->device_name_.c_str());
    }
  }

  void update() override {
    if (this->address_ == 0xFF)
      return;
    PowerState power = this->parent_->power_of(this->address_);
    if (power == POWER_UNKNOWN)
      return;
    bool on = (power == POWER_ON);
    if (on != this->state)
      this->publish_state(on);
  }

 protected:
  void write_state(bool state) override {
    if (this->address_ != 0xFF)
      this->parent_->send(this->address_, {state ? (uint8_t) 0x04 : (uint8_t) 0x36});  // Image View On / Standby
    this->publish_state(state);  // optimistic; reconciled by update()
  }

  HdmiCec *parent_{nullptr};
  uint8_t address_{0xFF};
  std::string device_name_;
};

}  // namespace hdmi_cec
}  // namespace esphome
