#include "status_indicator.h"
#include "esphome/components/network/util.h"
#include "esphome/core/log.h"
#include "esphome/core/application.h"

#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

#ifdef USE_ETHERNET
#include "esphome/components/ethernet/ethernet_component.h"
#endif

#ifdef USE_OPENTHREAD
#include "esphome/components/openthread/openthread.h"
#endif

#ifdef USE_MODEM
#include "esphome/components/modem/modem_component.h"
#endif

#ifdef USE_MQTT
#include "esphome/components/mqtt/mqtt_client.h"
#endif

#ifdef USE_API
#include "esphome/components/api/api_server.h"
#endif

namespace esphome {
namespace status_indicator {

static const char *const TAG = "status_indicator";

static bool has_network() {
#ifdef USE_ETHERNET
  if (ethernet::global_eth_component != nullptr)
    return true;
#endif

#ifdef USE_MODEM
  if (modem::global_modem_component != nullptr)
    return true;
#endif

#ifdef USE_WIFI
  if (wifi::global_wifi_component != nullptr)
    return true;
#endif

#ifdef USE_OPENTHREAD
  if (openthread::global_openthread_component != nullptr)
    return true;
#endif

#ifdef USE_HOST
  return true;  // Assume network is present for host-mode builds
#endif
  return false;
}

void StatusIndicator::dump_config() {
  ESP_LOGCONFIG(TAG, "Status Indicator triggers:");
  for (auto i = this->triggers_.begin(); i != this->triggers_.end(); i++) {
    ESP_LOGCONFIG(TAG, " * %s: %s", i->first.c_str(), i->second->get_info().c_str());
  }
}

void StatusIndicator::loop() {
  uint8_t new_state = App.get_app_state() & STATUS_LED_MASK;
  std::string new_status{""};

  if (new_state != this->last_app_state_) {
    ESP_LOGV(TAG, "New app state 0x%02X", new_state);
  }

  if ((new_state & STATUS_LED_ERROR) != 0u) {
    new_status = "on_app_error";
    this->status_.on_error = 1;
  } else if (this->status_.on_error) {
    new_status = "on_clear_app_error";
    this->status_.on_error = 0;
  }

  if (has_network()) {
#ifdef USE_WIFI
    if (new_status.empty() && wifi::global_wifi_component->is_ap_active()) {
      new_status = "on_wifi_ap_active";
      this->status_.on_wifi_ap = 1;
    } else if (this->status_.on_wifi_ap) {
      new_status = "on_wifi_ap_inactive";
      this->status_.on_wifi_ap = 0;
    }
#endif

    if (new_status.empty() && !network::is_connected()) {
      new_status = "on_network_disconnected";
      this->status_.on_network = 1;
    } else if (this->status_.on_network) {
      new_status = "on_network_connected";
      this->status_.on_network = 0;
    }

#ifdef USE_API
    if (new_status.empty() && api::global_api_server != nullptr && !api::global_api_server->is_connected()) {
      new_status = "on_api_disconnected";
      this->status_.on_api = 1;
    } else if (this->status_.on_api) {
      new_status = "on_api_connected";
      this->status_.on_api = 0;
    }
#endif

#ifdef USE_MQTT
    if (new_status.empty() && mqtt::global_mqtt_client != nullptr && !mqtt::global_mqtt_client->is_connected()) {
      new_status = "on_mqtt_disconnected";
      this->status_.on_mqtt = 1;
    } else if (this->status_.on_mqtt) {
      new_status = "on_mqtt_connected";
      this->status_.on_mqtt = 0;
    }
#endif
  }

  if (new_status.empty() && (new_state & STATUS_LED_WARNING) != 0u) {
    new_status = "on_app_warning";
    this->status_.on_warning = 1;
  } else if (this->status_.on_warning) {
    new_status = "on_clear_app_warning";
    this->status_.on_warning = 0;
  }

  if (this->last_status_ != new_status) {
    if (this->last_trigger_ != nullptr and this->last_trigger_->is_action_running() and !new_status.empty()) {
      this->last_trigger_->stop_action();
    }
    StatusTrigger *last_trigger = this->last_trigger_;

    if (!new_status.empty() && this->triggers_.count(new_status) == 1) {
      this->last_trigger_ = get_trigger(new_status);
    } else if (!this->stack_.empty()) {
      this->last_trigger_ = this->stack_.back();
      new_status = "on_custom_status";
    } else {
      this->last_trigger_ = get_trigger("on_turn_off");
      new_status = "on_turn_off";
    }

    if (last_trigger != this->last_trigger_) {
      ESP_LOGI(TAG, "<>> %s->%s", new_status.c_str(), this->last_trigger_->get_name().c_str());
      this->last_trigger_->trigger();
    }

    this->last_app_state_ = new_state;
    this->last_status_ = new_status;
  }
}

StatusTrigger *StatusIndicator::get_trigger(const std::string &key) {
  auto search = this->triggers_.find(key);
  if (search != this->triggers_.end()) {
    return search->second;
  } else {
    return nullptr;
  }
}

void StatusIndicator::set_trigger(const std::string &key, StatusTrigger *trigger) { this->triggers_[key] = trigger; }

void StatusIndicator::push_trigger(StatusTrigger *trigger) {
  this->pop_trigger(trigger, true);
  ESP_LOGD(TAG, "Push ID: %s", trigger->get_info().c_str());

  for (auto i = this->stack_.begin(); i != this->stack_.end(); ++i) {
    StatusTrigger *st = *i;
    if (trigger->get_priority() < st->get_priority()) {
      this->stack_.insert(i, trigger);
      this->last_status_ = "update me";
      ESP_LOGV(TAG, "After push:");
      log_triggers_();
      return;
    }
  }
  this->stack_.push_back(trigger);
  this->last_status_ = "update me";
  ESP_LOGV(TAG, "After push:");
  log_triggers_();
}

void StatusIndicator::pop_trigger(StatusTrigger *trigger, bool incl_group) {
  incl_group = incl_group && !trigger->get_group().empty();
  ESP_LOGD(TAG, "Pop by ID: %s || %s", trigger->get_info().c_str(), YESNO(incl_group));
  std::string group = trigger->get_group();
  for (auto i = this->stack_.begin(); i != this->stack_.end();) {
    StatusTrigger *st = *i;
    if ((incl_group && group == st->get_group()) || (trigger == st)) {
      this->stack_.erase(i);
      this->last_status_ = "update me";
    } else {
      ++i;
    }
  }
  ESP_LOGV(TAG, "After pop:");
  log_triggers_();
}

void StatusIndicator::pop_trigger(const std::string &group) {
  ESP_LOGD(TAG, "Pop by group: %s", group.c_str());

  for (auto i = this->stack_.begin(); i != this->stack_.end();) {
    StatusTrigger *st = *i;
    if (group == st->get_group()) {
      this->stack_.erase(i);
      this->last_status_ = "update me";
    } else {
      ++i;
    }
  }
  ESP_LOGV(TAG, "After pop:");
  log_triggers_();
}

void StatusIndicator::log_triggers_() {
  for (auto *st : this->stack_) {
    ESP_LOGV(TAG, "%s", st->get_info().c_str());
  }
  ESP_LOGV(TAG, "----------------------------- %d ----", this->stack_.size());
}

}  // namespace status_indicator
}  // namespace esphome
