# Message contracts

## BLE registration request example

Logical representation:

```text
version=1
flags=HAS_UUID or 0
hardware_id=6 raw bytes
current_uuid=16 bytes
nonce=uint32 little-endian
```

The UUID is only trusted when the response echoes the nonce and returns an accepted status.

## BLE telemetry example

```text
version=1
sample_count=2
hardware_id=48 F6 EE C4 CB E8
first_sample_sequence=32910
sample[0]=(ax, ay, az, gx, gy, gz)
sample[1]=(ax, ay, az, gx, gy, gz)
```

## MQTT/Kafka JSON

Canonical updated object:

```json
{
  "hardware_id": "48:F6:EE:C4:CB:E8",
  "ts": 1785221234567,
  "ax": 0.01,
  "ay": -0.02,
  "az": 0.99,
  "gx": 1.2,
  "gy": 0.5,
  "gz": -0.3
}
```

A normal gateway message is an array containing ten such objects.

## Derived reading values

The persistence consumer calculates:

```text
smv = sqrt(ax² + ay² + az²)
gyro_magnitude = sqrt(gx² + gy² + gz²)
```

## Classification output

A completed event can contain:

```json
{
  "event_type": "FALL | NEAR_MISS | NORMAL",
  "fall_type": "FFH | STF | null",
  "max_smv": 3.1,
  "max_rotation": 220.0,
  "freefall_duration": 0.22,
  "tilt_change": 75.0,
  "responsiveness": "still | active",
  "activity_before_event": "stationary | walking | running | jumping | unknown",
  "ts": 1785221234.5
}
```

## Compatibility warning

The current uploaded Kafka consumer documents and reads `device_id`; this contract documents the intended updated `hardware_id` form. Both producer and consumer must use the same key before end-to-end testing.
