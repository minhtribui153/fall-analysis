# Configuration reference

## Environment variables

| Variable | Used by | Purpose | Typical value |
|---|---|---|---|
| `DIR_DATA` | Docker Compose | Persistent runtime root | `./.data` |
| `POSTGRES_DB` | PostgreSQL and Python | Database name | `fall_analysis` |
| `POSTGRES_USER` | PostgreSQL and Python | Database user | project-specific |
| `POSTGRES_PASSWORD` | PostgreSQL and Python | Database password | secret |
| `POSTGRES_HOST` | Python | Database host | Pi IP on laptop; `localhost` on Pi |
| `POSTGRES_PORT` | Python | Database port | `5432` |
| `DATABASE_URL` | Python | Optional full DSN override | PostgreSQL URI |
| `PGADMIN_DEFAULT_EMAIL` | pgAdmin | Initial login | project-specific |
| `PGADMIN_DEFAULT_PASSWORD` | pgAdmin | Initial password | secret |
| `MQTT_HOST` | Gateway | Broker host | Pi IP |
| `MQTT_PORT` | Gateway | Broker port | `1883` |
| `MQTT_TOPIC` | Gateway | Output topic | `sensor-readings` |
| `MQTT_CLIENT_ID` | Gateway | MQTT client identifier | `tervanix-ble-gateway` |
| `MQTT_USERNAME` | Gateway | Optional broker username | empty in test setup |
| `MQTT_PASSWORD` | Gateway | Optional broker password | empty in test setup |
| `KAFKA_BROKER` | Consumer | Bootstrap server | `localhost:19092` |
| `KAFKA_TOPIC` | Consumer | Input topic | `sensor-readings` |
| `KAFKA_GROUP_ID` | Consumer | Consumer group | `postgres-writer` |

## Fixed protocol constants

| Constant | Value |
|---|---|
| BLE name | `TVX-REG` |
| BLE service UUID | `7db20000-6e8d-4d8d-a991-2cf812340000` |
| Registration UUID | `7db20001-6e8d-4d8d-a991-2cf812340000` |
| Telemetry UUID | `7db20002-6e8d-4d8d-a991-2cf812340000` |
| Sample rate | 100 Hz |
| Sample period | 10 ms |
| BLE samples per packet | 2 |
| MQTT readings per batch | 10 |
| PostgreSQL reading batch | 50 |

## Firmware board settings

`nesso_n1` uses:

- board profile `esp32-c6-devkitc-1`;
- ESP-IDF via `espressif32@6.13.0`;
- 2 MB flash metadata;
- large single-app partition;
- 1.5 MiB maximum application size;
- serial monitor at 115200 baud.
