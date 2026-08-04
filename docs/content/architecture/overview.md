# Architecture overview

**Source of truth:** `tervanix-n1/src`, `n1_registration_server.py`, `deployment`, and `detection`.

## Purpose

The system monitors motion using a waist-mounted Nesso N1. It captures accelerometer and gyroscope readings, transports them to a server pipeline, classifies activity/fall events, and stores raw and derived data in PostgreSQL.

## System context

![System context](../../assets/system-context.svg)

The system has three physical execution locations:

1. **Nesso N1 embedded device** — reads the BMI270 and acts as a BLE central/client.
2. **Gateway computer** — advertises the `TVX-REG` BLE service, registers devices, timestamps telemetry and publishes MQTT.
3. **Raspberry Pi server** — runs the messaging and persistence stack.

## Container view

![Container view](../../assets/container-view.svg)

| Container/component | Responsibility | Primary implementation |
|---|---|---|
| Nesso firmware | Identity, BLE discovery/registration, IMU sampling, telemetry transmission | `tervanix-n1/src` |
| BLE-MQTT gateway | BLE GATT server, UUID assignment, timestamping, MQTT batching | `n1_registration_server.py` |
| Mosquitto | Receives `sensor-readings` MQTT messages | `deployment/compose.mqtt-bridge.yaml` |
| Kafka Connect | Moves MQTT records into Kafka | `deployment/kafka-connect/mqtt_source_connector.json` |
| Kafka | Durable event stream for sensor batches | `deployment/compose.kafka.yaml` |
| Detection consumer | Calculates derived values, detects events, writes PostgreSQL | `detection/kafka_to_postgres.py` and `detection/detection.py` |
| PostgreSQL | Registration, sessions, raw readings, activity and event storage | `deployment/compose.postgres.yaml` plus external base schema |

## Architectural boundaries

### Embedded boundary

The Nesso intentionally has no Wi-Fi or MQTT responsibility in the active build. `tervanix-n1/src/CMakeLists.txt` compiles BLE, identity and IMU files only.

### Gateway boundary

The gateway owns:

- wall-clock timestamps;
- device registration records;
- conversion from compact binary BLE packets to JSON;
- batching ten readings per MQTT message.

### Server boundary

The Raspberry Pi owns infrastructure services and long-term persistence. Keeping Kafka and its Python consumer on the same Pi avoids remote Kafka listener complications caused by the current `EXTERNAL://localhost:19092` advertised listener.
