# Component responsibilities

## Nesso firmware (`tervanix-n1`)

### `main.cpp`
Boot sequence and runtime orchestration:
- initializes NVS and stored identity;
- waits for BLE registration;
- connects Wi-Fi and synchronizes time using SNTP;
- downloads/verifies the HAR model;
- starts encrypted MQTT;
- initializes the fixed offline journal and BMI270;
- starts sensor and offline uploader tasks.

The sensor task creates batches of **10 readings**. A failed/unavailable MQTT publish is written to the offline flash store.

### `imu/imu_sensor.cpp`
BMI270 accelerometer and gyroscope source. The runtime sensor period is driven by the model configuration; the server defaults to 100 Hz session sampling while HAR acquisition defaults to 40 ms/sample (~25 Hz) before resampling into the model tensor.

### `har_inference.cpp`
Runs TensorFlow Lite Micro inference. Model metadata defaults:
- model rate: 25 Hz;
- 50 model samples;
- 25-sample slide;
- 8 features;
- minimum confidence: 0.60.

When model confidence is below the threshold, `fallback()` performs an on-device heuristic classification using maximum acceleration and gyro magnitude.

### `offline_store.cpp`
Persistent flash-backed store-and-forward journal:
- fixed reservation: **512 KiB**;
- two-bank design with generation headers;
- FNV-1a checksum for corruption detection;
- each record stores up to 10 samples and optionally one prediction;
- on capacity pressure, default policy purges `unknown` batches first; if none exist, the oldest batch is removed;
- records are tombstoned after the MQTT client accepts the replayed QoS 1 message into its outbox.

### `mqtt_publisher.cpp`
Builds session telemetry/state JSON, encrypts it using AES-256-GCM, and publishes to session-specific topics. Telemetry and state publish calls use MQTT QoS 1.

### `model_manager.cpp`
Downloads an HMAC-authenticated model manifest and model binary over HTTP, verifies SHA-256, and persists model metadata. The firmware keeps 512 KiB reserved for the offline store so model storage cannot overlap the journal.

## Registration/model service (`detection/n1_registration_server.py`)

Provides:
- BLE service `TVX-REG`;
- device registration and worker/session assignment;
- bearer-token/session material;
- HTTP health/admin endpoints;
- authenticated model manifest and model binary endpoints.

Default session TTL is 28,800 seconds (8 hours).

## MQTT/PostgreSQL ingestion (`detection/mqtt_to_postgres.py`)

- subscribes to `tervanix/sessions/+/telemetry` by default;
- validates the encrypted envelope;
- reconstructs and verifies session bearer-token material;
- derives AES key as SHA-256(token) and decrypts AES-GCM telemetry;
- stores Nesso predictions as source `nesso_tflite`;
- computes `smv` and `gyro_magnitude` for each reading;
- invokes one `FallDetector` per session for **every** reading;
- stores server-rule classifications/activity logs as source `server_rules`;
- buffers readings in groups of 50 before inserts;
- uses `ON CONFLICT (session_id, sequence) DO NOTHING` for idempotent reading insertion.

## `detection.py`
Transparent threshold/rule detector using acceleration, rotation, free-fall, tilt, timing and post-event behaviour. It is independent from the Nesso TFLite prediction path.

## PostgreSQL
Canonical schema: `detection/schemas/database_schema.sql`.
