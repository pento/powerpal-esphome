#pragma once

#include "esphome/core/component.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/esp32_ble_tracker/esp32_ble_tracker.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/core/defines.h"

#ifdef USE_TIME
#include "esphome/components/time/real_time_clock.h"
#else
#include <ctime>
#endif

#ifdef USE_ESP32

#include <esp_gattc_api.h>

namespace esphome {
namespace powerpal_ble {

namespace espbt = esphome::esp32_ble_tracker;

static const espbt::ESPBTUUID POWERPAL_SERVICE_UUID =
    espbt::ESPBTUUID::from_raw("59DAABCD-12F4-25A6-7D4F-55961DCE4205");
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_MEASUREMENT_UUID =
    espbt::ESPBTUUID::from_raw("59DA0001-12F4-25A6-7D4F-55961DCE4205");
// Write [start_ts, end_ts] as two little-endian uint32 (8 bytes total) to ask
// the device to stream buffered measurements over the measurement characteristic.
// Without this write, no measurement notifications are sent.
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_MEASUREMENT_ACCESS_UUID =
    espbt::ESPBTUUID::from_raw("59DA0002-12F4-25A6-7D4F-55961DCE4205");
// Pulse notifications. Delivery is on a device-controlled ~390 ms-quantised
// grid that does NOT correspond 1:1 with actual meter pulses. The 4-byte LE
// uint32 payload IS the device's current inter-pulse interval in milliseconds;
// instantaneous power is `3.6e9 / (pulses_per_kwh * payload_ms)`. Independent
// of the batched-measurement stream.
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_PULSE_UUID =
    espbt::ESPBTUUID::from_raw("59DA0003-12F4-25A6-7D4F-55961DCE4205");
// Read/write the device's wall-clock (Unix epoch seconds, LE uint32, 4 bytes).
// The Powerpal needs this set after power-loss or it returns zeros from first_rec
// and tags new measurements with bogus timestamps.
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_TIME_UUID =
    espbt::ESPBTUUID::from_raw("59DA0004-12F4-25A6-7D4F-55961DCE4205");
// Read returns 8 bytes: [first_buffered_ts, last_buffered_ts] as two LE uint32.
// The Powerpal app calls this "firstRec" / "getFirstAndLastTimestamps".
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_FIRST_REC_UUID =
    espbt::ESPBTUUID::from_raw("59DA0005-12F4-25A6-7D4F-55961DCE4205");
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_LED_SENSITIVITY_UUID =
    espbt::ESPBTUUID::from_raw("59DA0008-12F4-25A6-7D4F-55961DCE4205");
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_APIKEY_UUID =
    espbt::ESPBTUUID::from_raw("59DA0009-12F4-25A6-7D4F-55961DCE4205");
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_PAIRING_CODE_UUID =
    espbt::ESPBTUUID::from_raw("59DA0011-12F4-25A6-7D4F-55961DCE4205");
static const espbt::ESPBTUUID POWERPAL_CHARACTERISTIC_READING_BATCH_SIZE_UUID =
    espbt::ESPBTUUID::from_raw("59DA0013-12F4-25A6-7D4F-55961DCE4205");

static const espbt::ESPBTUUID POWERPAL_BATTERY_SERVICE_UUID = espbt::ESPBTUUID::from_uint16(0x180F);
static const espbt::ESPBTUUID POWERPAL_BATTERY_CHARACTERISTIC_UUID = espbt::ESPBTUUID::from_uint16(0x2A19);

static const espbt::ESPBTUUID DEVICE_INFO_SERVICE_UUID = espbt::ESPBTUUID::from_uint16(0x180A);
static const espbt::ESPBTUUID FIRMWARE_REVISION_CHARACTERISTIC_UUID = espbt::ESPBTUUID::from_uint16(0x2A26);

static const uint8_t seconds_in_minute = 60;
static const float kw_to_w_conversion = 1000.0;

class Powerpal : public esphome::ble_client::BLEClientNode, public Component {
 public:
  void setup() override;
  void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                           esp_ble_gattc_cb_param_t *param) override;
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }
  void set_battery(sensor::Sensor *battery) { battery_ = battery; }
  void set_power_sensor(sensor::Sensor *power_sensor) { power_sensor_ = power_sensor; }
  void set_energy_sensor(sensor::Sensor *energy_sensor) { energy_sensor_ = energy_sensor; }
  void set_daily_energy_sensor(sensor::Sensor *daily_energy_sensor) { daily_energy_sensor_ = daily_energy_sensor; }
#ifdef USE_TIME
  void set_time(time::RealTimeClock *time) { time_ = time; }
#endif
  void set_pulses_per_kwh(float pulses_per_kwh) { pulses_per_kwh_ = pulses_per_kwh; }
  void set_pairing_code(uint32_t pairing_code) {
    pairing_code_[0] = (pairing_code & 0x000000FF);
    pairing_code_[1] = (pairing_code & 0x0000FF00) >> 8;
    pairing_code_[2] = (pairing_code & 0x00FF0000) >> 16;
    pairing_code_[3] = (pairing_code & 0xFF000000) >> 24;
  }
  void set_notification_interval(uint8_t reading_batch_size) { reading_batch_size_[0] = reading_batch_size; }
  void set_log_api_key(bool log_api_key) { log_api_key_ = log_api_key; }
  // When true, subscribe to the pulse characteristic and drive `power` from
  // the device's live inter-pulse interval. When false, the batched stream's
  // interval-average (notification_interval minutes) drives `power` — the
  // pre-live-power behavior.
  void set_live_power(bool enabled) { live_power_enabled_ = enabled; }

 protected:
  // Decode a 4-byte little-endian uint32 from the start of `data`. Callers are
  // responsible for verifying the buffer has at least 4 bytes available.
  static uint32_t read_le_u32_(const uint8_t *data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  }
  std::string pkt_to_hex_(const uint8_t *data, uint16_t len);
  void decode_(const uint8_t *data, uint16_t length);
  void parse_battery_(const uint8_t *data, uint16_t length);
  void parse_measurement_(const uint8_t *data, uint16_t length);
  void parse_apikey_(const uint8_t *data, uint16_t length);
  std::string serial_to_apikey_(const uint8_t *data, uint16_t length);
  void send_pairing_code_();
  // Discover GATT handles by UUID lookup after service discovery completes.
  // Returns false if any required characteristic is missing.
  bool discover_handles_();

  // Set true once the pairing-code write has been issued (from either GAP AUTH_CMPL or
  // GATTC SEARCH_CMPL — whichever fires first). Prevents duplicate writes when both fire.
  bool pairing_code_written_;
  // Set true once the Powerpal confirms the pairing-code write succeeded.
  bool authenticated_;

  sensor::Sensor *battery_{nullptr};
  sensor::Sensor *power_sensor_{nullptr};
  sensor::Sensor *energy_sensor_{nullptr};
  sensor::Sensor *daily_energy_sensor_{nullptr};
#ifdef USE_TIME
  optional<time::RealTimeClock *> time_{};
#endif
  uint16_t day_of_last_measurement_{0};

  uint8_t pairing_code_[4];
  uint8_t reading_batch_size_[4] = {0x01, 0x00, 0x00, 0x00};
  float pulses_per_kwh_;
  float pulse_multiplier_;
  // Numerator for the live-power formula: 3.6e9 / pulses_per_kwh. Precomputed
  // in setup() so each pulse notification only needs one division.
  float live_watts_numerator_{0.0f};
  uint64_t daily_pulses_{0};
  uint64_t total_pulses_{0};

  // GATT handles, populated by discover_handles_() after service discovery.
  // Zero means "not yet discovered or not present on this device".
  uint16_t pairing_code_char_handle_{0};
  uint16_t reading_batch_size_char_handle_{0};
  uint16_t measurement_char_handle_{0};
  uint16_t measurement_access_char_handle_{0};
  uint16_t first_rec_char_handle_{0};
  uint16_t time_char_handle_{0};

  uint16_t battery_char_handle_{0};
  uint16_t led_sensitivity_char_handle_{0};
  uint16_t firmware_char_handle_{0};
  // Char handle for the apikey/UUID characteristic (`59DA0009-...`). Read once
  // post-pair and logged when log_api_key_ is true. Diagnostic-only.
  uint16_t apikey_char_handle_{0};
  // Optional pulse-characteristic handle for the live-power subscription.
  // subscribe_live_power_() warns and falls back to batched mode if zero.
  uint16_t pulse_char_handle_{0};

  bool log_api_key_{false};
  // Always set from the YAML config via set_live_power() (see sensor.py), where
  // the effective default is true when a `power` sensor is configured. The
  // {false} here is only the fail-closed state before that setter runs.
  bool live_power_enabled_{false};

  // Track the most recent measurement timestamp we've received from the device.
  // Used to compute the start of the next measurement-range request so we don't
  // re-stream readings we've already published.
  uint32_t last_received_ts_{0};
  // The end timestamp of the currently-active measurement range request. When
  // a measurement arrives with this timestamp, the stream is finished and we
  // schedule the next poll.
  uint32_t requested_end_ts_{0};
  // True between requesting a measurement range and the stream ending. Used to
  // avoid issuing overlapping requests.
  bool measurement_request_in_flight_{false};

  // Issue a read of first_rec to learn the device's current [first_ts, last_ts]
  // and chain into write_measurement_access_().
  void poll_for_new_measurements_();
  // Write [start_ts, end_ts] (8 bytes LE) to measurement_access to start a stream.
  void write_measurement_access_(uint32_t start_ts, uint32_t end_ts);
  // Write the current Unix time to the device's time characteristic. The Powerpal
  // returns zeros from first_rec until its clock has been set.
  void write_device_time_();

  // Engage the live-power subscription if enabled. Called once per connection
  // after pairing has been confirmed. Clears live_power_enabled_ as a fallback
  // if the optional pulse characteristic is absent.
  void subscribe_live_power_();
  // Publish path for subscribe mode. `interval_millis` is the device-reported
  // inter-pulse interval from the pulse-characteristic payload. Power is
  // computed directly with no smoothing; the device pre-averages.
  void publish_live_watts_from_interval_(uint32_t interval_millis);
};

}  // namespace powerpal_ble
}  // namespace esphome

#endif
