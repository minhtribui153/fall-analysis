# Verification checklist

## Database

```sql
SELECT session_id, device_id, worker_id, active, sample_rate_hz
FROM monitoring.sessions
ORDER BY start_time DESC
LIMIT 5;
```

```sql
SELECT sequence, ts, ax, ay, az, gx, gy, gz, smv, gyro_magnitude
FROM monitoring.readings
ORDER BY reading_id DESC
LIMIT 20;
```

## Compare detector sources

```sql
SELECT ts, activity_state, confidence, source
FROM fall_detection.activity_log
ORDER BY ts DESC
LIMIT 30;
```

```sql
SELECT ts, event_type, fall_type, predicted_state, confidence,
       max_smv, max_rotation, responsiveness, source
FROM fall_detection.classification
ORDER BY ts DESC
LIMIT 30;
```

Expect both `nesso_tflite` and `server_rules` where each path produces output.

## Duplicate protection

```sql
SELECT session_id, sequence, COUNT(*)
FROM monitoring.readings
GROUP BY session_id, sequence
HAVING COUNT(*) > 1;
```

Expected: no rows.

## Offline replay ordering

For one test session:

```sql
SELECT sequence, ts
FROM monitoring.readings
WHERE session_id = '<session-uuid>'
ORDER BY sequence;
```

Check that cached records retain their original order and event timestamps.

## MQTT QoS

If the deployment claims broker-to-consumer QoS 1, verify `MQTT_QOS=1` in the ingestion environment. Firmware publish calls already use QoS 1.
