# End-to-end data flow

![Telemetry flow](../../assets/telemetry-flow.svg)

## 1. Registration

1. Nesso discovers BLE service `TVX-REG`.
2. The registration service validates/creates device identity and associates an approved worker.
3. An active `monitoring.sessions` row is created with expiring bearer-token metadata.
4. Worker/session/network context is provisioned to the Nesso and persisted locally.

## 2. Model provisioning

1. After Wi-Fi connection, the Nesso requests `/v1/device/model/manifest` using signed session headers.
2. The server returns an HMAC-protected canonical manifest containing model hash, size, classes and inference metadata.
3. The Nesso validates the manifest, downloads `/v1/device/model/binary` when needed and verifies SHA-256.

## 3. Sensing and edge inference

1. IMU samples enter the Nesso pipeline.
2. HAR uses approximately 25 Hz acquisition, a 50-sample tensor and a 25-sample slide.
3. If confidence is below the configured threshold, the firmware's local `fallback()` heuristic replaces the predicted state.
4. The prediction retains the original model confidence value and is attached to telemetry/state messages.

## 4. Live publish or offline store

1. Ten raw readings form one telemetry batch.
2. If MQTT is connected and accepts the publish, the batch leaves through encrypted MQTT.
3. Otherwise the full batch, including its prediction if present, is persisted to the flash journal.
4. `offline_uploader_task` replays the oldest active record after MQTT reconnects.
5. The flash copy is tombstoned only after the MQTT client accepts the replay publish into its QoS 1 outbox.

The journal preserves each sample's original `sequence`, `source_uptime_ms` and wall-clock `timestamp_ms`.

## 5. MQTT security and authentication

Telemetry is published to:

`tervanix/sessions/<32-hex-session-uuid>/telemetry`

The outer JSON carries session/worker identifiers plus `nonce`, `ciphertext`, and `tag`. The bearer token is not transmitted in the MQTT JSON.

## 6. Server processing

1. `mqtt_to_postgres.py` receives and decrypts the envelope.
2. The embedded Nesso prediction, if present, is written to `activity_log`; fall/near-miss states also create a `classification` row with source `nesso_tflite`.
3. Every raw reading is transformed to SMV and gyro magnitude.
4. Every raw reading is also passed to the server-side `FallDetector`.
5. Rule events/activity changes are written separately with source `server_rules`.
6. Raw readings are flushed in 50-row batches.

## 7. Persistence/retrieval boundary

PostgreSQL becomes the source for downstream monitoring. The uploaded repository does not contain the dashboard frontend/API source, so dashboard-specific polling behaviour is outside this repository's code boundary.
