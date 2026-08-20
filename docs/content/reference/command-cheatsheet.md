# Command cheat sheet

## Firmware
```bash
cd tervanix-n1
pio run -e nesso_n1
```

## Registration/model service
```bash
python detection/n1_registration_server.py
```

## MQTT -> PostgreSQL consumer
```bash
python detection/mqtt_to_postgres.py
```

## PostgreSQL quick checks
```sql
SELECT COUNT(*) FROM monitoring.readings;
SELECT * FROM fall_detection.activity_log ORDER BY ts DESC LIMIT 20;
SELECT * FROM fall_detection.classification ORDER BY ts DESC LIMIT 20;
```

## Offline replay check
```sql
SELECT sequence, ts
FROM monitoring.readings
WHERE session_id='<session-uuid>'
ORDER BY sequence;
```
