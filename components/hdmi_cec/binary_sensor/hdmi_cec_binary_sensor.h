#pragma once

#include <string>

#include "../hdmi_cec.h"
#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec {

enum HdmiCecBinaryType : uint8_t {
  HDMI_CEC_BINARY_MUTE = 0,
  HDMI_CEC_BINARY_POWER,
};

// Mirrors a tracked device's mute or power state from the registry.
class HdmiCecBinarySensor : public binary_sensor::BinarySensor, public PollingComponent {
 public:
  void set_parent(HdmiCec *parent) { this->parent_ = parent; }
  void set_address(uint8_t address) { this->address_ = address; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_type(HdmiCecBinaryType type) { this->type_ = type; }

  void setup() override {
    if (this->address_ == 0xFF && !this->device_name_.empty()) {
      uint8_t resolved = this->parent_->address_of(this->device_name_);
      if (resolved != 0xFF)
        this->address_ = resolved;
      else
        ESP_LOGW("hdmi_cec.binary_sensor", "unknown device '%s'", this->device_name_.c_str());
    }
  }

  void update() override {
    if (this->address_ == 0xFF) return;
    bool value = (this->type_ == HDMI_CEC_BINARY_MUTE) ? this->parent_->mute_of(this->address_)
                                                       : (this->parent_->power_of(this->address_) == POWER_ON);
    if (!this->has_state() || this->state != value) this->publish_state(value);
  }

 protected:
  HdmiCec *parent_{nullptr};
  uint8_t address_{0xFF};
  HdmiCecBinaryType type_{HDMI_CEC_BINARY_MUTE};
  std::string device_name_;
};

}  // namespace hdmi_cec
}  // namespace esphome
