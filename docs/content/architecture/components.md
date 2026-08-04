# Component responsibilities

## Nesso firmware

### `main.cpp`

Bootstraps NVS and the identity store, starts the BLE client, waits for registration, initializes the BMI270, then creates:

- a **100 Hz sampling task**;
- a **five-second status task**.

The sampling task groups two samples per BLE packet. A batch is dropped when telemetry is unavailable rather than stored for later retransmission.

### `ble_advertiser.cpp`

Despite the historical filename, this module makes the Nesso a BLE **central/client**. It:

- scans for the name `TVX-REG` or the registration service UUID;
- connects and negotiates an ATT MTU of 64;
- discovers the registration and telemetry characteristics;
- sends a registration request and reads the response;
- writes telemetry without response;
- automatically returns to scanning after disconnection.

### `device_identity.cpp`

Reads the station MAC address with `esp_read_mac(..., ESP_MAC_WIFI_STA)` and uses those six bytes as the canonical `hardware_id`. This does not mean the firmware uses Wi-Fi; it only uses the stable MAC source exposed by ESP-IDF.

### `identity_store.cpp`

Persists the assigned 16-byte node UUID in NVS namespace `identity`, key `node_uuid`.

### `imu/imu_sensor.cpp`

Initializes I²C and the BMI270 with:

- SDA: GPIO 10;
- SCL: GPIO 8;
- address: `0x68`;
- bus speed: 400 kHz;
- accelerometer: 100 Hz, ±2 g;
- gyroscope: 100 Hz, ±2000 degrees/s.

## BLE-MQTT gateway

### BLE registration

`n1_registration_server.py` creates a GATT server containing one service and two characteristics. It creates and maintains the `node_registrations` table and assigns a node UUID to each `hardware_id`.

### Telemetry processing

The gateway validates each 60-byte packet, verifies that the device registered during the current process, checks its sample sequence, timestamps both samples, and accumulates readings by `hardware_id`.

### MQTT bridge

Ten readings are serialized as a compact JSON array and published with QoS 1 to `sensor-readings`.

## Messaging infrastructure

### Mosquitto

Receives MQTT from the laptop gateway. Its configuration is mounted from `${DIR_DATA}/instances/mosquitto/config` and must include a listener on port 1883.

### Kafka Connect

Uses `io.confluent.connect.mqtt.MqttSourceConnector` to subscribe to MQTT topic `sensor-readings` and produce Kafka topic `sensor-readings`.

### Kafka

Runs as a single-node KRaft broker with:

- internal listener `kafka:9092` for containers;
- external listener `localhost:19092` for Pi-hosted processes.

## Detection and persistence

### `kafka_to_postgres.py`

Consumes Kafka messages, accepts either a JSON array or one object, manages per-device sessions, buffers 50 raw readings, computes SMV and gyro magnitude, invokes `FallDetector`, and inserts records into PostgreSQL.

> The uploaded file still expects `device_id`; the intended updated contract uses `hardware_id`. See [Known limitations](known-limitations.md).

### `detection.py`

Contains a threshold-based `ActivityClassifier` and `FallDetector`. It is deliberately not an ML model. It uses acceleration magnitude, rotation magnitude, orientation change, sustained down-time, freefall duration and recovery behaviour.

### PostgreSQL

Stores device identity, open sessions, raw readings, classifications and activity changes. The complete base schema is not present in this repository; only a later migration is included.
