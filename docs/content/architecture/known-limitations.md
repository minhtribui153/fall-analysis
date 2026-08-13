# Known limitations and repository inconsistencies

This page documents the uploaded repository **without changing code**. These items should be resolved before treating a fresh checkout as production-ready.

## 1. `hardware_id` and `device_id` are inconsistent

The gateway and BLE packets now use `hardware_id`, but the following still use `device_id`:

- `detection/kafka_to_postgres.py`;
- `deployment/migrations/002_fix_timestamps_and_view.sql`;
- `RUNBOOK.md`;
- comments and inactive legacy MQTT files.

As written, the gateway publishes `hardware_id` while the Kafka consumer reads `msg["device_id"]`, so the current checkout will reject or fail to parse live messages unless the consumer has been changed locally.

## 2. Base database schema is absent

The repository includes only migration `002_fix_timestamps_and_view.sql`. It assumes the main schemas and tables already exist. A new deployment cannot create the full database from this repository alone.

## 3. Uploaded gateway contains the old Bless permission spelling

The uploaded `n1_registration_server.py` uses `GATTAttributePermissions.writeable`; the installed Bless API observed during testing uses `writable`. A locally fixed copy may work while a fresh checkout fails.

## 4. Windows Bless advertising fix is environment-local

During Windows testing, Bless's installed `server.py` required a compatibility change for the WinRT advertising overload. That modification lives inside the virtual environment, not in the repository or dependency pin. Recreating the environment may reintroduce the failure.

## 5. Telemetry loss is detected but not recovered

Write Without Response can produce sequence-gap warnings. The system logs gaps but does not reorder, retransmit or mark individual database rows as missing.

## 6. IMU units require validation

The detector expects acceleration in g and gyroscope values in degrees per second. `imu_sensor.cpp` explicitly notes that output units still need a stationary sanity check.

## 7. Thresholds have limited validation

Fall/near-miss thresholds are based on a small set of staged recordings. The FFH versus STF split is specifically marked as not validated against a real height fall.

## 8. Legacy code remains in the repository

Wi-Fi/MQTT C++ files and Arduino prototypes remain for reference but are not part of the active firmware build. They may contain outdated naming and architecture assumptions.

## 9. Runtime secrets and generated data were included in the uploaded archive

The archive contains `.env`, `.venv`, `.pio`, `.cache`, `.git` and persisted database/pgAdmin data. These should not be committed or redistributed. The documented-source package generated with these docs excludes them.
