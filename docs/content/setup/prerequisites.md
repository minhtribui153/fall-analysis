# Prerequisites

## Hardware
- Arduino Nesso N1 with BMI270 IMU;
- Wi-Fi network reachable by Nesso and server;
- BLE-capable host for registration;
- server/Raspberry Pi for MQTT, PostgreSQL and Python services.

## Software
- PlatformIO / ESP-IDF toolchain for `tervanix-n1`;
- Python 3 and `detection/requirements.txt`;
- PostgreSQL;
- Mosquitto or compatible MQTT broker.

Kafka/Kafka Connect are optional legacy/alternative infrastructure and are not prerequisites for the active `mqtt_to_postgres.py` path.
