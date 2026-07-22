#pragma once

#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include <string>

#include "../hdmi_cec.h"

namespace esphome {
namespace hdmi_cec {

enum HdmiCecTextType : uint8_t {
  HDMI_CEC_TEXT_OSD_NAME = 0,
  HDMI_CEC_TEXT_POWER_STATE,
};

// Mirrors a tracked device's OSD name or power state (as text) from the registry.
class HdmiCecTextSensor : public text_sensor::TextSensor, public PollingComponent {
 public:
  void set_parent(HdmiCec *parent) { this->parent_ = parent; }
  void set_address(uint8_t address) { this->address_ = address; }
  void set_device_name(const std::string &name) { this->device_name_ = name; }
  void set_type(HdmiCecTextType type) { this->type_ = type; }

  void setup() override {
    if (this->address_ == 0xFF && !this->device_name_.empty()) {
      uint8_t resolved = this->parent_->address_of(this->device_name_);
      if (resolved != 0xFF)
        this->address_ = resolved;
      else
        ESP_LOGW("hdmi_cec.text_sensor", "unknown device '%s'", this->device_name_.c_str());
    }
  }

  void update() override {
    if (this->address_ == 0xFF)
      return;
    std::string value;
    if (this->type_ == HDMI_CEC_TEXT_OSD_NAME) {
      value = this->parent_->osd_name_of(this->address_);
      if (value.empty())
        return;  // not learned yet
    } else {
      switch (this->parent_->power_of(this->address_)) {
        case POWER_ON:
          value = "on";
          break;
        case POWER_STANDBY:
          value = "standby";
          break;
        default:
          value = "unknown";
          break;
      }
    }
    if (!this->has_state() || this->state != value)
      this->publish_state(value);
  }

 protected:
  HdmiCec *parent_{nullptr};
  uint8_t address_{0xFF};
  HdmiCecTextType type_{HDMI_CEC_TEXT_OSD_NAME};
  std::string device_name_;
};

}  // namespace hdmi_cec
}  // namespace esphome
