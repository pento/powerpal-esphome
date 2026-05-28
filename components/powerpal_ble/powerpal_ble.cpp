#include "powerpal_ble.h"
#include "esphome/core/log.h"
#include "esphome/core/hal.h"

#ifdef USE_ESP32

namespace esphome {
namespace powerpal_ble {

static const char *const TAG = "powerpal_ble";

void Powerpal::dump_config() {
  ESP_LOGCONFIG(TAG, "POWERPAL");
  LOG_SENSOR(" ", "Battery", this->battery_);
  LOG_SENSOR(" ", "Power", this->power_sensor_);
  LOG_SENSOR(" ", "Daily Energy", this->daily_energy_sensor_);
  LOG_SENSOR(" ", "Total Energy", this->energy_sensor_);
}

void Powerpal::setup() {
  this->pairing_code_written_ = false;
  this->authenticated_ = false;
  this->pulse_multiplier_ = ((seconds_in_minute * this->reading_batch_size_[0]) / (this->pulses_per_kwh_ / kw_to_w_conversion));
  this->live_watts_numerator_ = 3'600'000'000.0f / this->pulses_per_kwh_;
  ESP_LOGD(TAG, "pulse_multiplier_: %f, live_watts_numerator_: %f", this->pulse_multiplier_, this->live_watts_numerator_);
}

std::string Powerpal::pkt_to_hex_(const uint8_t *data, uint16_t len) {
  char buf[64];
  memset(buf, 0, 64);
  for (int i = 0; i < len; i++)
    sprintf(&buf[i * 2], "%02x", data[i]);
  std::string ret = buf;
  return ret;
}

void Powerpal::decode_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
}

void Powerpal::parse_battery_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "Battery: DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
  if (length == 1) {
    this->battery_->publish_state(data[0]);
  }
}

std::string Powerpal::serial_to_apikey_(const uint8_t *data, uint16_t length) {
  const char *hexmap[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "a", "b", "c", "d", "e", "f"};
  std::string api_key;
  for (int i = 0; i < length; i++) {
    if (i == 4 || i == 6 || i == 8 || i == 10) {
      api_key.append("-");
    }
    api_key.append(hexmap[(data[i] & 0xF0) >> 4]);
    api_key.append(hexmap[data[i] & 0x0F]);
  }
  return api_key;
}

void Powerpal::parse_apikey_(const uint8_t *data, uint16_t length) {
  ESP_LOGI(TAG, "Powerpal API key (for cloud-API access): %s", this->serial_to_apikey_(data, length).c_str());
}

void Powerpal::parse_measurement_(const uint8_t *data, uint16_t length) {
  ESP_LOGD(TAG, "Measurement: DEC(%d): 0x%s", length, this->pkt_to_hex_(data, length).c_str());
  if (length < 6) {
    return;
  }

  time_t unix_time = static_cast<time_t>(read_le_u32_(data));

  uint16_t pulses_within_interval = data[4];
  pulses_within_interval += data[5] << 8;

  float avg_watts_within_interval = pulses_within_interval * this->pulse_multiplier_;

  ESP_LOGI(TAG, "Timestamp: %u, Pulses: %u, Average Watts within interval: %.3f W",
           static_cast<unsigned>(unix_time), static_cast<unsigned>(pulses_within_interval),
           avg_watts_within_interval);

  // When the live-power subscription is engaged, it owns the `power` sensor
  // and we suppress the batched interval-average publish here. Mixing the
  // two would bias HA's time-weighted hourly mean at interval boundaries.
  if (this->power_sensor_ != nullptr && !this->live_power_enabled_) {
    this->power_sensor_->publish_state(avg_watts_within_interval);
  }

  if (this->energy_sensor_ != nullptr) {
    this->total_pulses_ += pulses_within_interval;
    float energy = this->total_pulses_ / this->pulses_per_kwh_;
    this->energy_sensor_->publish_state(energy);
  }

  if (this->daily_energy_sensor_ != nullptr) {
    this->daily_pulses_ += pulses_within_interval;
    float energy = this->daily_pulses_ / this->pulses_per_kwh_;
    this->daily_energy_sensor_->publish_state(energy);

#ifdef USE_TIME
    auto *time_ = *this->time_;
    ESPTime date_of_measurement = time_->now();
    if (date_of_measurement.is_valid()) {
      if (this->day_of_last_measurement_ == 0) { this->day_of_last_measurement_ = date_of_measurement.day_of_year;}
      else if (this->day_of_last_measurement_ != date_of_measurement.day_of_year) {
        this->daily_pulses_ = 0;
        this->day_of_last_measurement_ = date_of_measurement.day_of_year;
      }
    } else {
#else
      struct tm *date_of_measurement = ::localtime(&unix_time);
      if (this->day_of_last_measurement_ == 0) { this->day_of_last_measurement_ = date_of_measurement->tm_yday + 1;}
      else if (this->day_of_last_measurement_ != date_of_measurement->tm_yday + 1) {
        this->daily_pulses_ = 0;
        this->day_of_last_measurement_ = date_of_measurement->tm_yday + 1;
      }
#endif
#ifdef USE_TIME
    }
#endif
  }

  this->last_received_ts_ = static_cast<uint32_t>(unix_time);

  if (this->requested_end_ts_ != 0 && this->last_received_ts_ >= this->requested_end_ts_) {
    ESP_LOGD(TAG, "Reached end of requested range (ts=%u >= end=%u); scheduling next poll",
             this->last_received_ts_, this->requested_end_ts_);
    this->measurement_request_in_flight_ = false;
    this->requested_end_ts_ = 0;
    uint32_t poll_delay_ms = this->reading_batch_size_[0] * 60u * 1000u;
    this->set_timeout("powerpal_poll", poll_delay_ms,
                      [this]() { this->poll_for_new_measurements_(); });
  }
}

bool Powerpal::discover_handles_() {
  struct Lookup {
    const espbt::ESPBTUUID &service_uuid;
    const espbt::ESPBTUUID &char_uuid;
    const char *name;
    uint16_t *out;
    bool required;
  };
  const Lookup table[] = {
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_PAIRING_CODE_UUID, "pairing_code",
       &this->pairing_code_char_handle_, true},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_READING_BATCH_SIZE_UUID, "reading_batch_size",
       &this->reading_batch_size_char_handle_, true},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_MEASUREMENT_UUID, "measurement",
       &this->measurement_char_handle_, true},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_MEASUREMENT_ACCESS_UUID, "measurement_access",
       &this->measurement_access_char_handle_, true},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_FIRST_REC_UUID, "first_rec", &this->first_rec_char_handle_,
       true},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_TIME_UUID, "time", &this->time_char_handle_, true},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_LED_SENSITIVITY_UUID, "led_sensitivity",
       &this->led_sensitivity_char_handle_, false},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_APIKEY_UUID, "apikey", &this->apikey_char_handle_, false},
      {POWERPAL_SERVICE_UUID, POWERPAL_CHARACTERISTIC_PULSE_UUID, "pulse", &this->pulse_char_handle_, false},
      {POWERPAL_BATTERY_SERVICE_UUID, POWERPAL_BATTERY_CHARACTERISTIC_UUID, "battery", &this->battery_char_handle_,
       false},
      {DEVICE_INFO_SERVICE_UUID, FIRMWARE_REVISION_CHARACTERISTIC_UUID, "firmware", &this->firmware_char_handle_,
       false},
  };

  bool ok = true;
  for (const auto &entry : table) {
    auto *chr = this->parent()->get_characteristic(entry.service_uuid, entry.char_uuid);
    if (chr == nullptr) {
      if (entry.required) {
        ESP_LOGE(TAG, "Required characteristic '%s' not found on device", entry.name);
        ok = false;
      } else {
        ESP_LOGD(TAG, "Optional characteristic '%s' not found", entry.name);
      }
      continue;
    }
    *entry.out = chr->handle;
    ESP_LOGI(TAG, "Discovered '%s' handle: 0x%04x", entry.name, chr->handle);
  }
  return ok;
}

void Powerpal::write_device_time_() {
  if (this->time_char_handle_ == 0) {
    ESP_LOGW(TAG, "Cannot set device time: time handle not discovered");
    return;
  }
#ifdef USE_TIME
  if (!this->time_.has_value()) {
    ESP_LOGW(TAG, "Cannot set device time: no time_id configured");
    return;
  }
  auto *clock = *this->time_;
  ESPTime now = clock->now();
  if (!now.is_valid()) {
    ESP_LOGW(TAG, "Cannot set device time: time source is not synced yet");
    return;
  }
  uint32_t unix_ts = static_cast<uint32_t>(now.timestamp);
  uint8_t payload[4] = {static_cast<uint8_t>(unix_ts & 0xFF), static_cast<uint8_t>((unix_ts >> 8) & 0xFF),
                        static_cast<uint8_t>((unix_ts >> 16) & 0xFF), static_cast<uint8_t>((unix_ts >> 24) & 0xFF)};
  ESP_LOGI(TAG, "Setting Powerpal time to %u", unix_ts);
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                         this->time_char_handle_, sizeof(payload), payload, ESP_GATT_WRITE_TYPE_RSP,
                                         ESP_GATT_AUTH_REQ_NONE);
  if (status) {
    ESP_LOGW(TAG, "Error sending write request for time, status=%d", status);
  }
#else
  ESP_LOGW(TAG, "Cannot set device time: USE_TIME not enabled");
#endif
}

void Powerpal::poll_for_new_measurements_() {
  if (this->measurement_request_in_flight_) {
    ESP_LOGD(TAG, "Skipping poll; request already in flight");
    return;
  }
  if (this->first_rec_char_handle_ == 0) {
    ESP_LOGW(TAG, "Cannot poll: first_rec handle not discovered");
    return;
  }
  this->measurement_request_in_flight_ = true;
  ESP_LOGD(TAG, "Polling for new measurements: reading first_rec");
  auto status = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                        this->first_rec_char_handle_, ESP_GATT_AUTH_REQ_NONE);
  if (status) {
    ESP_LOGW(TAG, "Error sending read request for first_rec, status=%d", status);
    this->measurement_request_in_flight_ = false;
  }
}

void Powerpal::write_measurement_access_(uint32_t start_ts, uint32_t end_ts) {
  if (this->measurement_access_char_handle_ == 0) {
    ESP_LOGW(TAG, "Cannot request measurements: measurement_access handle not discovered");
    this->measurement_request_in_flight_ = false;
    return;
  }
  uint8_t payload[8];
  payload[0] = start_ts & 0xFF;
  payload[1] = (start_ts >> 8) & 0xFF;
  payload[2] = (start_ts >> 16) & 0xFF;
  payload[3] = (start_ts >> 24) & 0xFF;
  payload[4] = end_ts & 0xFF;
  payload[5] = (end_ts >> 8) & 0xFF;
  payload[6] = (end_ts >> 16) & 0xFF;
  payload[7] = (end_ts >> 24) & 0xFF;
  this->requested_end_ts_ = end_ts;
  ESP_LOGI(TAG, "Requesting measurement stream [%u, %u]", start_ts, end_ts);
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                         this->measurement_access_char_handle_, sizeof(payload), payload,
                                         ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status) {
    ESP_LOGW(TAG, "Error sending write request for measurement_access, status=%d", status);
    this->measurement_request_in_flight_ = false;
    this->requested_end_ts_ = 0;
  }
}

void Powerpal::subscribe_live_power_() {
  if (!this->live_power_enabled_) {
    return;
  }
  if (this->pulse_char_handle_ == 0) {
    // Older firmware doesn't expose the pulse char. Disable live mode so the
    // batched stream's interval-average publish resumes as a fallback rather
    // than leaving `power` permanently silent.
    ESP_LOGW(TAG, "Live power requested but pulse characteristic not discovered; falling back to batched average");
    this->live_power_enabled_ = false;
    return;
  }
  ESP_LOGI(TAG, "Subscribing to pulse notifications for live power");
  auto status = esp_ble_gattc_register_for_notify(this->parent_->get_gattc_if(), this->parent_->get_remote_bda(),
                                                  this->pulse_char_handle_);
  if (status) {
    // Same fallback as the missing-handle path above: if we can't subscribe,
    // resume the batched publish rather than leaving `power` silent.
    ESP_LOGW(TAG, "register_for_notify(pulse) failed, status=%d; falling back to batched average", status);
    this->live_power_enabled_ = false;
  }
}

void Powerpal::publish_live_watts_from_interval_(uint32_t interval_millis) {
  if (this->power_sensor_ == nullptr || interval_millis == 0) {
    return;
  }
  // `interval_millis` is the device's current inter-pulse interval from the
  // pulse-characteristic payload, in ms. Power = energy / time:
  //   watts = 3.6e9 / (ppk * interval_ms) = live_watts_numerator_ / interval_ms.
  float watts = this->live_watts_numerator_ / static_cast<float>(interval_millis);
  ESP_LOGD(TAG, "Live watts: %.2f (interval=%u ms)", watts, interval_millis);
  this->power_sensor_->publish_state(watts);
}

void Powerpal::send_pairing_code_() {
  if (this->pairing_code_written_) {
    return;
  }
  if (this->pairing_code_char_handle_ == 0) {
    // Handles not discovered yet; SEARCH_CMPL_EVT will retry once they are.
    return;
  }
  this->pairing_code_written_ = true;
  ESP_LOGI(TAG, "[%s] Writing pairing code to Powerpal", this->parent_->address_str());
  auto status = esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                         this->pairing_code_char_handle_, sizeof(this->pairing_code_),
                                         this->pairing_code_, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
  if (status) {
    ESP_LOGW(TAG, "Error sending write request for pairing_code, status=%d", status);
    // Allow a retry on the next triggering event.
    this->pairing_code_written_ = false;
  }
}

void Powerpal::gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                   esp_ble_gattc_cb_param_t *param) {
  switch (event) {
    case ESP_GATTC_DISCONNECT_EVT: {
      this->pairing_code_written_ = false;
      this->authenticated_ = false;
      this->measurement_request_in_flight_ = false;
      this->requested_end_ts_ = 0;
      this->cancel_timeout("powerpal_poll");
      break;
    }
    case ESP_GATTC_SEARCH_CMPL_EVT: {
      if (!this->discover_handles_()) {
        // Required characteristic missing; can't proceed.
        return;
      }
      // Defensive fallback: send the pairing code now that services are discovered,
      // in case ESP_GAP_BLE_AUTH_CMPL_EVT never fires (the Powerpal does not require
      // BLE-level bonding under current Bluedroid / IDF v5).
      this->send_pairing_code_();
      break;
    }
    case ESP_GATTC_READ_CHAR_EVT: {
      ESP_LOGD(TAG, "[%s] ESP_GATTC_READ_CHAR_EVT (Received READ)", this->parent_->address_str());
      if (param->read.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error reading char at handle %d, status=%d", param->read.handle, param->read.status);
        break;
      }
      // reading batch size
      if (param->read.handle == this->reading_batch_size_char_handle_) {
        ESP_LOGD(TAG, "Received reading_batch_size read event");
        this->decode_(param->read.value, param->read.value_len);
        if (param->read.value_len == 4) {
          // Always write back the configured batch size. Even if the device
          // already reports the matching value, the write itself appears to be
          // what kicks off measurement streaming on some Powerpal firmware
          // versions. The WRITE_CHAR_EVT handler will then subscribe.
          auto status =
              esp_ble_gattc_write_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                       this->reading_batch_size_char_handle_, sizeof(this->reading_batch_size_),
                                       this->reading_batch_size_, ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
          if (status) {
            ESP_LOGW(TAG, "Error sending write request for batch_size, status=%d", status);
          }
        }
        break;
      }

      // battery
      if (param->read.handle == this->battery_char_handle_) {
        ESP_LOGD(TAG, "Received battery read event");
        this->parse_battery_(param->read.value, param->read.value_len);
        break;
      }

      // firmware
      if (param->read.handle == this->firmware_char_handle_) {
        ESP_LOGD(TAG, "Received firmware read event");
        this->decode_(param->read.value, param->read.value_len);
        break;
      }

      // led sensitivity
      if (param->read.handle == this->led_sensitivity_char_handle_) {
        ESP_LOGD(TAG, "Received led sensitivity read event");
        this->decode_(param->read.value, param->read.value_len);
        break;
      }

      // apikey (diagnostic only — read once when log_api_key: true)
      if (param->read.handle == this->apikey_char_handle_) {
        this->parse_apikey_(param->read.value, param->read.value_len);
        break;
      }

      // first_rec — 8 bytes: [first_buffered_ts, last_buffered_ts] (two LE uint32).
      // Triggers a write to measurement_access to request the next slice of measurements.
      if (param->read.handle == this->first_rec_char_handle_) {
        ESP_LOGD(TAG, "Received first_rec read event");
        this->decode_(param->read.value, param->read.value_len);
        if (param->read.value_len < 8) {
          ESP_LOGW(TAG, "first_rec returned %d bytes, expected 8", param->read.value_len);
          this->measurement_request_in_flight_ = false;
          this->set_timeout("powerpal_poll", this->reading_batch_size_[0] * 60u * 1000u,
                            [this]() { this->poll_for_new_measurements_(); });
          break;
        }
        uint32_t first_ts = read_le_u32_(&param->read.value[0]);
        uint32_t last_ts = read_le_u32_(&param->read.value[4]);
        ESP_LOGI(TAG, "Device buffer: first_ts=%u, last_ts=%u", first_ts, last_ts);

        if (first_ts == 0 || last_ts == 0) {
          ESP_LOGD(TAG, "Device buffer is empty; will retry");
          this->measurement_request_in_flight_ = false;
          this->set_timeout("powerpal_poll", this->reading_batch_size_[0] * 60u * 1000u,
                            [this]() { this->poll_for_new_measurements_(); });
          break;
        }

        const uint32_t interval_seconds = static_cast<uint32_t>(this->reading_batch_size_[0]) * seconds_in_minute;
        uint32_t start_ts;
        if (this->last_received_ts_ == 0) {
          // First request after (re)connect — only fetch the most recent reading
          // so we transition into live data quickly.
          start_ts = (last_ts > interval_seconds) ? (last_ts - interval_seconds) : first_ts;
        } else {
          start_ts = this->last_received_ts_ + interval_seconds;
        }
        if (start_ts < first_ts) {
          start_ts = first_ts;
        }

        if (start_ts > last_ts) {
          ESP_LOGD(TAG, "No new measurements (start=%u > last=%u); will retry", start_ts, last_ts);
          this->measurement_request_in_flight_ = false;
          this->set_timeout("powerpal_poll", this->reading_batch_size_[0] * 60u * 1000u,
                            [this]() { this->poll_for_new_measurements_(); });
          break;
        }

        this->write_measurement_access_(start_ts, last_ts);
        break;
      }

      break;
    }

    case ESP_GATTC_WRITE_CHAR_EVT: {
      ESP_LOGD(TAG, "[%s] ESP_GATTC_WRITE_CHAR_EVT (Write confirmed)", this->parent_->address_str());
      if (param->write.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "Error writing value to char at handle %d, status=%d", param->write.handle, param->write.status);
        // Allow a retry of the pairing-code write on the next triggering event.
        if (param->write.handle == this->pairing_code_char_handle_) {
          this->pairing_code_written_ = false;
        }
        // Recover from a failed measurement_access write so we don't get stuck waiting forever.
        if (param->write.handle == this->measurement_access_char_handle_) {
          this->measurement_request_in_flight_ = false;
          this->requested_end_ts_ = 0;
          this->set_timeout("powerpal_poll", this->reading_batch_size_[0] * 60u * 1000u,
                            [this]() { this->poll_for_new_measurements_(); });
        }
        break;
      }

      if (param->write.handle == this->pairing_code_char_handle_ && !this->authenticated_) {
        this->authenticated_ = true;

        // Set the device's clock first. The Powerpal returns zeros from first_rec
        // until it knows its own time, and tags new measurements with bogus
        // timestamps. BLE GATT serialises operations so this queues ahead of the
        // subsequent reads.
        this->write_device_time_();

        auto read_reading_batch_size_status =
            esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                    this->reading_batch_size_char_handle_, ESP_GATT_AUTH_REQ_NONE);
        if (read_reading_batch_size_status) {
          ESP_LOGW(TAG, "Error sending read request for reading batch size, status=%d", read_reading_batch_size_status);
        }

        if (this->battery_ != nullptr) {
          // read battery
          auto read_battery_status = esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                                             this->battery_char_handle_, ESP_GATT_AUTH_REQ_NONE);
          if (read_battery_status) {
            ESP_LOGW(TAG, "Error sending read request for battery, status=%d", read_battery_status);
          }
          // Enable notifications for battery
          auto notify_battery_status = esp_ble_gattc_register_for_notify(
              this->parent_->get_gattc_if(), this->parent_->get_remote_bda(), this->battery_char_handle_);
          if (notify_battery_status) {
            ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify failed, status=%d",
                     this->parent_->address_str(), notify_battery_status);
          }
        }

        // read firmware version
        auto read_firmware_status =
            esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                    this->firmware_char_handle_, ESP_GATT_AUTH_REQ_NONE);
        if (read_firmware_status) {
          ESP_LOGW(TAG, "Error sending read request for firmware, status=%d", read_firmware_status);
        }

        // read led sensitivity
        auto read_led_sensitivity_status =
            esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                    this->led_sensitivity_char_handle_, ESP_GATT_AUTH_REQ_NONE);
        if (read_led_sensitivity_status) {
          ESP_LOGW(TAG, "Error sending read request for led sensitivity, status=%d", read_led_sensitivity_status);
        }

        // read apikey if diagnostic logging is enabled (one-shot, value is logged
        // by parse_apikey_ then never re-read until reconnect).
        if (this->log_api_key_) {
          auto read_apikey_status =
              esp_ble_gattc_read_char(this->parent()->get_gattc_if(), this->parent()->get_conn_id(),
                                      this->apikey_char_handle_, ESP_GATT_AUTH_REQ_NONE);
          if (read_apikey_status) {
            ESP_LOGW(TAG, "Error sending read request for apikey, status=%d", read_apikey_status);
          }
        }

        // Engage the live-power subscription if enabled.
        this->subscribe_live_power_();

        break;
      }
      if (param->write.handle == this->reading_batch_size_char_handle_) {
        // reading batch size is now set correctly so subscribe to measurement notifications
        auto status = esp_ble_gattc_register_for_notify(this->parent_->get_gattc_if(), this->parent_->get_remote_bda(),
                                                        this->measurement_char_handle_);
        if (status) {
          ESP_LOGW(TAG, "[%s] esp_ble_gattc_register_for_notify failed, status=%d",
                   this->parent_->address_str(), status);
        }
        break;
      }

      if (param->write.handle == this->measurement_access_char_handle_) {
        ESP_LOGD(TAG, "measurement_access write confirmed; stream should now begin");
        break;
      }

      if (param->write.handle == this->time_char_handle_) {
        ESP_LOGD(TAG, "device time write confirmed");
        break;
      }

      ESP_LOGW(TAG, "[%s] Missed all handle matches: %d",
               this->parent_->address_str(), param->write.handle);
      break;
    }  // ESP_GATTC_WRITE_CHAR_EVT

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      if (param->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGW(TAG, "register_for_notify failed for handle 0x%04x, status=%d",
                 param->reg_for_notify.handle, param->reg_for_notify.status);
        // If the pulse subscription couldn't be established, disable live
        // mode so parse_measurement_ resumes publishing the batched average
        // rather than leaving `power` silent until the next reconnect.
        if (param->reg_for_notify.handle == this->pulse_char_handle_) {
          ESP_LOGW(TAG, "Falling back to batched average for `power`");
          this->live_power_enabled_ = false;
        }
        break;
      }
      ESP_LOGD(TAG, "register_for_notify succeeded for handle 0x%04x", param->reg_for_notify.handle);
      if (param->reg_for_notify.handle == this->measurement_char_handle_) {
        // Subscription is live; kick off the first measurement-range request.
        this->poll_for_new_measurements_();
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT: {
      ESP_LOGD(TAG, "[%s] Received Notification", this->parent_->address_str());

      // battery
      if (param->notify.handle == this->battery_char_handle_) {
        ESP_LOGD(TAG, "Received battery notify event");
        this->parse_battery_(param->notify.value, param->notify.value_len);
        break;
      }

      // measurement
      if (param->notify.handle == this->measurement_char_handle_) {
        ESP_LOGD(TAG, "Received measurement notify event");
        this->parse_measurement_(param->notify.value, param->notify.value_len);
        break;
      }

      // pulse — the 4-byte LE u32 payload is the device's current inter-pulse
      // interval in milliseconds (verified empirically: at steady ~475 W the
      // value sits at ~2370 ms; at ~5.6 kW it drops to ~200 ms; the implied
      // power matches the batched 60 s average across the range). The BLE
      // delivery cadence is separate (multiples of ~390 ms) and doesn't
      // correspond to actual pulse arrivals, so we ignore it and trust the
      // payload directly.
      if (param->notify.handle == this->pulse_char_handle_) {
        if (param->notify.value_len < 4) {
          ESP_LOGW(TAG, "Pulse notify too short (%d bytes): 0x%s", param->notify.value_len,
                   this->pkt_to_hex_(param->notify.value, param->notify.value_len).c_str());
          break;
        }
        uint32_t interval_ms = read_le_u32_(param->notify.value);
        ESP_LOGD(TAG, "Pulse notify interval_ms=%u", interval_ms);
        this->publish_live_watts_from_interval_(interval_ms);
        break;
      }
      break;
    }
    default:
      break;
  }
}

void Powerpal::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  switch (event) {
    // This event is sent once authentication has completed
    case ESP_GAP_BLE_AUTH_CMPL_EVT: {
      if (param->ble_security.auth_cmpl.success) {
        this->send_pairing_code_();
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace powerpal_ble
}  // namespace esphome

#endif
