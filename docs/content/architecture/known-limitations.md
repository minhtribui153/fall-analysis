# Known limitations and current inconsistencies

## 1. Intended low-confidence routing does not match code

If the intended design is "confidence < threshold -> use server-side `detection.py` as the fallback", the current merged source does not implement that routing.

Current behaviour:
1. `har_inference.cpp` calls local `fallback(max_accel, max_gyro)` when confidence is low;
2. the resulting state is still published as source `nesso_tflite` with the original confidence;
3. `mqtt_to_postgres.py` processes **every** raw sample through server `FallDetector`, regardless of confidence.

This is the highest-priority behaviour/documentation mismatch to resolve.

## 2. MQTT subscriber defaults to QoS 0

The Nesso publishes telemetry with QoS 1, but `mqtt_to_postgres.py` defaults `MQTT_QOS` to 0 for its subscription. If end-to-end at-least-once broker-to-consumer delivery is required, set and test `MQTT_QOS=1`.

## 3. Offline journal is finite

The fixed 512 KiB reservation prevents unbounded flash growth. When a bank fills, queued unknown batches are purged first by default; if no unknown batch is available, the oldest batch is dropped.

## 4. Replay acknowledgement is enqueue-level, not application persistence-level

The firmware tombstones an offline record once the MQTT client accepts the QoS 1 publish into its local outbox. That is stronger than immediate deletion, but it is not the same as receiving confirmation that PostgreSQL committed the batch.

## 5. Detection validation remains limited

`detection.py` thresholds are based on staged data and should not be presented as production-validated FFH/STF accuracy without a larger labelled evaluation set.

## 6. Kafka files are stale relative to the active path

Kafka/Kafka Connect assets remain in `deployment/`. Do not infer that they are part of the current runtime merely because the files exist.

## 7. `.env.example` is stale

It still labels MQTT variables as "BLE gateway -> MQTT" and exposes legacy `MQTT_TOPIC=sensor-readings`/`MQTT_CLIENT_ID`. The active Nesso firmware uses session-scoped topics and the Python consumer uses `MQTT_TELEMETRY_SUBSCRIPTION`/`MQTT_QOS` configuration.

## 8. Dashboard not included in this archive

The uploaded repository contains no current NestJS/Next.js dashboard source. Dashboard documentation must be maintained with the separate dashboard project or merged later.
