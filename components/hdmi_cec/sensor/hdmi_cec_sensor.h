#pragma once

#include <string>

#include "../hdmi_cec.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

namespace esphome {
namespace hdmi_cec {

// Mirrors a tracked device's audio volume (0-100) from the registry.
class HdmiCecSensor : public sensor::Sensor, public PollingComponent {
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
        ESP_LOGW("hdmi_cec.sensor", "unknown device '%s'", this->device_name_.c_str());
    }
  }

  void update() override {
    if (this->address_ == 0xFF) return;
    uint8_t volume = this->parent_->volume_of(this->address_);
    if (volume == 0xFF) return;  // unknown
    if (!this->has_state() || this->state != volume) this->publish_state(volume);
  }

 protected:
  HdmiCec *parent_{nullptr};
  uint8_t address_{0xFF};
  std::string device_name_;
};

}  // namespace hdmi_cec
}  // namespace esphome
