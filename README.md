# esphome-powerpal

ESPHome external component for the [Powerpal](https://www.powerpal.net/)
BLE energy monitor. Reads power, energy and battery directly over BLE
— no phone app, no Powerpal cloud.

## Origin

Refactored from
[`WeekendWarrior1/esphome@powerpal_ble`](https://github.com/WeekendWarrior1/esphome/tree/powerpal_ble),
which was a fork of ESPHome itself and stopped compiling after
ESPHome 2025.6.x. This repo packages the same protocol logic as a
modern self-contained external_components folder, so it builds
against current ESPHome (2026.x+) and tracks upstream's BLE APIs
without forking.

Differences from the upstream fork:

- Modern external_components layout — no ESPHome fork required.
- ESP-IDF framework (the original Arduino-framework path is the
  source of the upstream's bit-rot).
- Cloud-upload path removed (`USE_HTTP_REQUEST`, device-id /
  apikey readback, JSON POST to `readings.powerpal.net`). HA is the
  destination; round-tripping through Powerpal's cloud is not
  wanted.
- Defensive fallback: the pairing-code write fires from
  `ESP_GATTC_SEARCH_CMPL_EVT` *as well as* `ESP_GAP_BLE_AUTH_CMPL_EVT`,
  with a single-write gate. The Powerpal does not require BLE-level
  bonding, and `AUTH_CMPL` may not fire at all under current
  Bluedroid / IDF v5; relying on it alone causes the connection to
  stall.

## Hardware

Any ESP32. Tested on M5Stack AtomS3 Lite (ESP32-S3FN8). Place within
BLE range of the Powerpal — a few metres through one or two interior
walls.

## Usage

```yaml
esp32:
  board: m5stack-atoms3-lite
  variant: esp32s3
  framework:
    type: esp-idf

esp32_ble_tracker:

ble_client:
  - mac_address: XX:XX:XX:XX:XX:XX   # from the Powerpal sticker
    id: powerpal

time:
  - platform: homeassistant
    id: homeassistant_time

external_components:
  - source: github://pento/esphome-powerpal@main
    components: [powerpal_ble]

sensor:
  - platform: powerpal_ble
    ble_client_id: powerpal
    pairing_code: 123456              # from the Powerpal app / info card
    notification_interval: 1          # minutes between batches (1–60)
    pulses_per_kwh: 1000              # confirm against Powerpal app setup
    time_id: homeassistant_time
    power:
      name: "Powerpal Power"
    daily_energy:
      name: "Powerpal Daily Energy"
    energy:
      name: "Powerpal Total Energy"
    battery_level:
      name: "Powerpal Battery"
```

## Pairing

The Powerpal supports a single BLE pairing at a time. Unpair it from
your phone first (Bluetooth settings → forget device). Note the
6-digit pairing code somewhere persistent before doing so — losing
it means a factory reset.

After the ESP32 boots, look for the `[powerpal_ble]` log lines:
`Writing pairing code to Powerpal` →
`ESP_GATTC_WRITE_CHAR_EVT (Write confirmed)` → service reads and
notification subscriptions. First measurement arrives within
`notification_interval` minutes.

## License

MIT.
