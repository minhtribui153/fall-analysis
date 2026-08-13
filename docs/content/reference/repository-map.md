# Repository map

```text
fall-analysis/
├── n1_registration_server.py      BLE registration + telemetry + MQTT gateway
├── requirements.txt               Python dependencies
├── .env.example                   Shared configuration template
├── Makefile                       Routes deployment commands
├── README.md                      Original short project introduction
├── RUNBOOK.md                     Older bring-up notes; partly outdated
├── detection/
│   ├── detection.py               Activity and fall logic
│   └── kafka_to_postgres.py       Kafka consumer and DB writer
├── deployment/
│   ├── compose.postgres.yaml      PostgreSQL + pgAdmin
│   ├── compose.kafka.yaml         Kafka + Kafka UI
│   ├── compose.mqtt-bridge.yaml   Mosquitto + Kafka Connect
│   ├── kafka-connect/
│   │   └── mqtt_source_connector.json
│   └── migrations/
│       └── 002_fix_timestamps_and_view.sql
├── tervanix-n1/
│   ├── platformio.ini             ESP-IDF PlatformIO environments
│   ├── include/                   Project C++ interfaces/protocols
│   └── src/                       Active and legacy firmware sources
├── arduino/                       Early Arduino/TFLite prototypes
├── BLE_template                   Earlier BLE peripheral example
└── docs/                          This documentation
```

## Active custom code

| File | Status |
|---|---|
| `tervanix-n1/src/main.cpp` | Active |
| `tervanix-n1/src/ble_advertiser.cpp` | Active; name is historical |
| `tervanix-n1/src/device_identity.cpp` | Active |
| `tervanix-n1/src/identity_store.cpp` | Active |
| `tervanix-n1/src/imu/imu_sensor.cpp` | Active |
| `n1_registration_server.py` | Active gateway |
| `detection/detection.py` | Active detector |
| `detection/kafka_to_postgres.py` | Intended active consumer; naming mismatch remains |

## Inactive or legacy custom code

- `tervanix-n1/src/wifi_manager.cpp`
- `tervanix-n1/src/mqtt_publisher.cpp`
- matching legacy headers
- `arduino/*`
- `BLE_template`

They are not included in active firmware `idf_component_register(SRCS ...)`.

## Third-party/generated directories

Do not review or commit as custom source:

- `.git/`
- `.venv/`
- `tervanix-n1/.pio/`
- `tervanix-n1/.cache/`
- `tervanix-n1/managed_components/`
- `deployment/.data/`
- `compile_commands.json`
