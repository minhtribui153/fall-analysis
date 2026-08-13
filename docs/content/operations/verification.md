# Verification checklist

## Network

From laptop:

```powershell
Test-NetConnection <PI_IP> -Port 1883
Test-NetConnection <PI_IP> -Port 5432
```

## PostgreSQL registration

Use the updated identity column:

```sql
SELECT hardware_id, node_uuid, active, last_seen_at
FROM node_registrations
ORDER BY registered_at DESC
LIMIT 5;
```

## MQTT

```bash
docker exec -it sg-nyp-a1-eg2a17-mosquitto \
  mosquitto_sub -h localhost -t sensor-readings -v
```

Confirm messages contain `hardware_id`, `ts`, and all six axes.

## Kafka Connect

```bash
curl http://localhost:8083/connectors/nesso-sensor-mqtt-source/status
```

Expected:

```text
connector: RUNNING
task 0: RUNNING
```

## Kafka

```bash
docker exec -it sg-nyp-a1-eg2a17-kafka \
  /opt/kafka/bin/kafka-console-consumer.sh \
  --bootstrap-server localhost:9092 \
  --topic sensor-readings \
  --max-messages 1
```

## Raw readings

```sql
SELECT COUNT(*) AS total_readings, MAX(ts) AS latest_reading
FROM monitoring.readings;
```

Run twice several seconds apart. The count and latest timestamp should advance.

## Sensor sanity

```sql
SELECT ts, ax, ay, az, gx, gy, gz, smv, gyro_magnitude
FROM monitoring.readings
ORDER BY ts DESC
LIMIT 20;
```

At rest, acceleration magnitude should be approximately 1 g and gyro magnitude should be near zero.

## Derived data

```sql
SELECT session_id, ts, activity_state
FROM fall_detection.activity_log
ORDER BY ts DESC
LIMIT 10;
```

```sql
SELECT session_id, ts, event_type, fall_type, max_smv, max_rotation
FROM fall_detection.classification
ORDER BY ts DESC
LIMIT 10;
```

## Data quality

Keep sequence-gap warnings visible during testing. A successful database insert does not prove that every 100 Hz sample arrived.
