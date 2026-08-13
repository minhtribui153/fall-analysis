# Command cheat sheet

## Firmware

```powershell
cd tervanix-n1
pio run -e nesso_n1
pio run -e nesso_n1 -t upload
pio device monitor -e nesso_n1
```

## Pi containers

```bash
sudo make init
sudo make deploy up postgres kafka mqtt-bridge
sudo make deploy down mqtt-bridge kafka postgres
docker ps
```

## Connector

```bash
curl http://localhost:8083/connector-plugins
curl -X POST -H 'Content-Type: application/json' \
  --data @deployment/kafka-connect/mqtt_source_connector.json \
  http://localhost:8083/connectors
curl http://localhost:8083/connectors/nesso-sensor-mqtt-source/status
```

## MQTT inspection

```bash
docker exec -it sg-nyp-a1-eg2a17-mosquitto \
  mosquitto_sub -h localhost -t sensor-readings -v
```

## Kafka inspection

```bash
docker exec -it sg-nyp-a1-eg2a17-kafka \
  /opt/kafka/bin/kafka-console-consumer.sh \
  --bootstrap-server localhost:9092 --topic sensor-readings
```

## PostgreSQL

```bash
docker exec -it sg-nyp-a1-eg2a17-postgres \
  psql -U "$POSTGRES_USER" -d "$POSTGRES_DB"
```

```sql
SELECT hardware_id, node_uuid, active, last_seen_at
FROM node_registrations
ORDER BY registered_at DESC LIMIT 5;

SELECT COUNT(*), MAX(ts) FROM monitoring.readings;
```

## Gateway

```powershell
.\.venv\Scripts\Activate.ps1
python n1_registration_server.py
```

## Connectivity

```powershell
Test-NetConnection <PI_IP> -Port 1883
Test-NetConnection <PI_IP> -Port 5432
```
