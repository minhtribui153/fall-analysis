# Database model

Canonical source: `detection/schemas/database_schema.sql`.

## Schemas and tables

### `identification`
- `workers`: durable worker identity (`worker_id`, UUID, name, active state).
- `devices`: physical Nesso `device_id`, optional BLE address, registration/last-seen timestamps.
- `registration_audit`: approval/rejection history.

### `public`
- `node_registrations`: device approval/link state and current worker/session association.

### `monitoring`
- `sessions`: active worker-device monitoring session, bearer-token hash/expiry, sample rate and lifecycle state.
- `readings`: raw six-axis values plus derived `smv` and `gyro_magnitude`.

### `fall_detection`
- `activity_log`: changing activity state from either `nesso_tflite` or `server_rules`.
- `classification`: NORMAL/NEAR_MISS/FALL evidence, optional FFH/STF subtype, confidence/model metadata or rule evidence.
- `alerts`: persistent alert and acknowledgement state.

## Important constraints

- one active registration per device;
- one active session per device;
- one active session per worker;
- unique `(session_id, sequence)` raw reading;
- unique device prediction per `(session_id, prediction_sequence)`;
- foreign keys tie sessions/readings/events back to device/worker identity.

## Why sequence uniqueness matters

The Nesso can replay cached telemetry after reconnection, and MQTT QoS 1 can redeliver a publication. `(session_id, sequence)` makes raw-reading persistence idempotent.

## Event sources

The schema deliberately stores source information so Nesso ML output and server-rule output can coexist rather than being silently merged.
