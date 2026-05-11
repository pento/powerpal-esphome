# powerpal-esphome

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
  `ESP_GATTC_SEARCH_CMPL_EVT` _as well as_ `ESP_GAP_BLE_AUTH_CMPL_EVT`,
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
  board: esp32-s3-devkitc-1
  variant: esp32s3
  framework:
    type: esp-idf

esp32_ble_tracker:

ble_client:
  - mac_address: XX:XX:XX:XX:XX:XX # see "Finding the MAC" below
    id: powerpal

time:
  - platform: homeassistant
    id: homeassistant_time

external_components:
  - source: github://pento/powerpal-esphome@main
    components: [powerpal_ble]

sensor:
  - platform: powerpal_ble
    ble_client_id: powerpal
    pairing_code: 123456 # from the Powerpal app / info card
    notification_interval: 1 # minutes between batches (1–60)
    pulses_per_kwh: 1000 # confirm against Powerpal app setup
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

## Finding the MAC

The MAC isn't printed on the Powerpal hardware. Three options:

- **Android**: nRF Connect → scan → look for `powerpal NNNNNNNN`. MAC
  is shown directly under the name (Android exposes real BLE MACs;
  iOS/macOS replace them with per-app UUIDs that don't work here).
- **The ESP32 itself**: flash this component with a placeholder MAC.
  On boot, `esp32_ble_tracker:` logs every advertisement it sees,
  including the Powerpal's MAC. Update the YAML and re-flash.
- **Powerpal currently connected to the phone won't appear in scans**
  (single-pair behaviour). Force-quit the Powerpal app first, wait
  ~30 seconds, then scan.

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

## Diagnostic: log_api_key

Optional `log_api_key: true` (default `false`) on the `powerpal_ble`
sensor platform. When set, the component reads the Powerpal's
cloud-API key from BLE characteristic `59DA0009-...` once after
pairing succeeds and logs it at INFO level:

```text
[I][powerpal_ble:NNN]: Powerpal API key (for cloud-API access): XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
```

Useful if you want to query Powerpal's `readings.powerpal.net` API
(e.g. to backfill historical data into Home Assistant) and your app
version doesn't expose "Generate an API Key" under Guidance. Toggle
on, OTA, capture from logs, toggle back off, OTA again. The key
identifies your account to Powerpal's servers and is otherwise the
same value on every read.

## License

MIT.
