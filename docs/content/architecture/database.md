# Database model

## Schema ownership

The repository contains two different sources of database structure:

1. `n1_registration_server.py` creates `node_registrations` if it does not exist.
2. The rest of the schemas are expected to come from an external base schema that is not present in the uploaded repository.

The included `deployment/migrations/002_fix_timestamps_and_view.sql` is a follow-up migration, not a complete schema.

## Canonical identity

The updated design uses `hardware_id` as the physical-device key. It is formatted as an uppercase colon-delimited MAC string:

```text
AA:BB:CC:DD:EE:FF
```

The same physical Nesso also has a registration `node_uuid`.

## `node_registrations`

Created by the gateway with these fields:

| Column | Purpose |
|---|---|
| `registration_id` | Surrogate primary key |
| `hardware_id` | Physical Nesso identifier |
| `node_uuid` | Unique UUID assigned by registration |
| `active` | Whether this is the current registration |
| `registered_at` | Initial registration time |
| `last_seen_at` | Last accepted registration refresh |
| `delinked_at` | Time a registration was deactivated |

A partial unique index permits only one active registration per `hardware_id`.

## Downstream tables used by the consumer

The intended hardware-ID schema must provide equivalent fields for:

### `devices.identification`

Used to associate `hardware_id`, `node_uuid` and `last_seen_at`.

### `monitoring.sessions`

Used to store an open session for one hardware device and its sample rate.

### `monitoring.readings`

The consumer inserts:

```text
session_id, ts, ax, ay, az, gx, gy, gz, smv, gyro_magnitude
```

### `fall_detection.classification`

The consumer inserts event type, optional fall type, peak acceleration, peak rotation, freefall duration, tilt change, responsiveness and prior activity.

### `fall_detection.activity_log`

Receives a row only when the classified activity state changes.

## Timestamp requirements

The consumer creates timezone-aware UTC `datetime` objects. Database timestamp columns receiving them should be `TIMESTAMPTZ`.

## Current migration warning

`002_fix_timestamps_and_view.sql` still refers to `device_id`, including foreign keys and the dashboard view. It must be reconciled with the updated `hardware_id` schema before being treated as the current migration source.
