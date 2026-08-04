# End-to-end data flow

![Telemetry flow](../../assets/telemetry-flow.svg)

## 1. Boot and identity

1. The Nesso initializes NVS.
2. `IdentityStore` loads the previously assigned node UUID, when present.
3. The firmware reads the six-byte MAC-derived `hardware_id`.

## 2. BLE discovery and connection

1. The Nesso scans indefinitely.
2. It accepts an advertisement that contains either:
   - local name `TVX-REG`; or
   - service UUID `7db20000-6e8d-4d8d-a991-2cf812340000`.
3. It connects and requests an ATT MTU of 64.
4. It discovers the registration and telemetry characteristics.

## 3. Registration

1. The Nesso writes a 28-byte registration request.
2. The gateway looks for an active `hardware_id` + UUID match.
3. If the stored UUID is valid, it is accepted and `last_seen_at` is updated.
4. Otherwise the gateway deactivates the old registration and creates a new UUID.
5. The Nesso reads a 21-byte response, validates the nonce and saves the UUID in NVS.

## 4. Sampling and BLE telemetry

1. BMI270 readings are sampled at 100 Hz.
2. Two samples are placed in each 60-byte packet.
3. The packet is written using GATT Write Without Response.
4. The sequence counter advances even when a local write fails, allowing the gateway to detect a future gap.

## 5. Gateway conversion

1. The gateway verifies packet length, version and sample count.
2. It rejects telemetry from a device that did not register during the current gateway process.
3. It anchors the newest sample to gateway receipt time.
4. It spaces samples by 10 ms.
5. It converts each reading into a JSON object containing `hardware_id`, timestamp and six sensor axes.
6. It publishes every ten readings as one MQTT JSON array.

## 6. MQTT to Kafka

1. Mosquitto accepts the `sensor-readings` MQTT message.
2. Kafka Connect subscribes to that MQTT topic.
3. The connector writes the raw value to Kafka topic `sensor-readings` using byte-array converters.

## 7. Kafka to PostgreSQL

1. `kafka_to_postgres.py` parses the JSON payload.
2. It resolves or creates an active monitoring session.
3. It calculates:
   - signal magnitude vector (SMV);
   - gyroscope magnitude.
4. Every 50 readings are inserted into `monitoring.readings`.
5. Activity changes are inserted into `fall_detection.activity_log`.
6. Completed fall/near-miss event classifications are inserted into `fall_detection.classification`.

## Failure isolation

Each boundary can be tested independently:

| Boundary | Verification |
|---|---|
| Nesso → gateway | Gateway receives writes on telemetry UUID |
| Gateway → MQTT | `mosquitto_sub` sees JSON arrays |
| MQTT → Kafka | Kafka console consumer sees the same payload |
| Kafka → consumer | Python process receives without parse warnings |
| Consumer → PostgreSQL | `monitoring.readings` row count increases |
