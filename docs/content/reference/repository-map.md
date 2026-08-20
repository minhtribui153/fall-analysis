# Repository map

```text
fall-analysis/
├── detection/
│   ├── n1_registration_server.py   BLE registration + admin/model HTTP service
│   ├── mqtt_to_postgres.py         Active encrypted MQTT -> PostgreSQL consumer
│   ├── detection.py                Server rule-based activity/fall detector
│   ├── model_hash.py               Model manifest/hash helper
│   └── schemas/database_schema.sql Canonical relational schema
├── tervanix-n1/
│   ├── src/main.cpp                Runtime orchestration
│   ├── src/har_inference.cpp       TFLite inference + local low-confidence heuristic
│   ├── src/offline_store.cpp       512 KiB flash journal
│   ├── src/mqtt_publisher.cpp      AES-GCM MQTT telemetry
│   ├── src/model_manager.cpp       Model manifest/download/storage
│   ├── src/registration_client.cpp BLE registration client
│   ├── src/wifi_manager.cpp        Wi-Fi connection
│   └── src/imu/imu_sensor.cpp      BMI270 driver
├── deployment/                     PostgreSQL plus legacy Kafka/MQTT bridge assets
└── docs/                           Project documentation
```

## Legacy / non-active items

- Kafka/Kafka Connect compose and connector files remain available for older/alternative architecture tests.
- `arduino/` contains older sketches/model artifacts and is not the primary current ESP-IDF/PlatformIO firmware.
