# Architecture overview

## Active architecture

The repository implements a hybrid edge/server worker-safety pipeline.

1. **BLE registration and provisioning**: `detection/n1_registration_server.py` advertises `TVX-REG`. It registers a Nesso, associates it with a worker/session, provisions Wi-Fi/session credentials and exposes authenticated model endpoints.
2. **Edge sensing and inference**: `tervanix-n1` samples the BMI270, runs TensorFlow Lite Micro HAR inference, and creates 10-reading telemetry batches.
3. **Offline resilience**: failed MQTT batches are written to a fixed 512 KiB flash journal and replayed when MQTT becomes available.
4. **MQTT transport**: telemetry is application-layer encrypted with AES-256-GCM and published to session-specific MQTT topics.
5. **Server processing**: `detection/mqtt_to_postgres.py` authenticates/decrypts messages, computes SMV and gyro magnitude, records device predictions, runs `FallDetector` on every raw reading, and writes PostgreSQL.
6. **Persistence**: PostgreSQL stores workers, devices, registrations, sessions, readings, activity logs, classifications and alerts.

![Container view](../../assets/container-view.svg)

## Key clarification: ML confidence

The current code contains **two rule mechanisms**:

- `tervanix-n1/src/har_inference.cpp`: if TFLite confidence is below `min_confidence` (default `0.60`), an **on-device magnitude heuristic** replaces the state with `stationary`, `falling_from_height`, `trip`, `jumping`, `walking`, or `unknown`.
- `detection/detection.py`: an independent server-side `FallDetector` processes every raw sensor reading received by `mqtt_to_postgres.py`.

Therefore the current implementation is not a conditional "low confidence -> server rules only" router.

## Legacy infrastructure

Kafka, Kafka Connect and the older MQTT bridge compose files remain in `deployment/`, but the active Python ingestion program is `detection/mqtt_to_postgres.py` and does not consume Kafka.
