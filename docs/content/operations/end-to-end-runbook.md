# End-to-end runbook

Follow this order so every stage has a clear pass/fail point.

## A. Pi infrastructure

```bash
cd ~/fall-analysis
sudo make init
sudo make deploy up postgres
sudo make deploy up kafka
sudo make deploy up mqtt-bridge
docker ps
```

Confirm PostgreSQL, Kafka, Mosquitto and Kafka Connect are running.

## B. Database

Confirm the base schema exists and the intended `hardware_id` changes are applied. Do not rely on migration 002 alone.

## C. Connector

```bash
curl http://localhost:8083/connector-plugins
curl http://localhost:8083/connectors/nesso-sensor-mqtt-source/status
```

The plugin must exist; connector and task must be `RUNNING`.

## D. Pi consumer

```bash
source .venv-pipeline/bin/activate
python detection/kafka_to_postgres.py
```

Leave it running.

## E. Live inspection terminals

MQTT:

```bash
docker exec -it sg-nyp-a1-eg2a17-mosquitto \
  mosquitto_sub -h localhost -p 1883 -t sensor-readings -v
```

Kafka:

```bash
docker exec -it sg-nyp-a1-eg2a17-kafka \
  /opt/kafka/bin/kafka-console-consumer.sh \
  --bootstrap-server localhost:9092 \
  --topic sensor-readings
```

## F. Laptop gateway

Update `.env` with the Pi IP, confirm ports 1883 and 5432, then:

```powershell
python n1_registration_server.py
```

Wait for both MQTT connection and BLE gateway startup.

## G. Nesso

Power/reset the already-flashed Nesso. Monitor serial output if available.

## H. Validate in order

1. Nesso registers in `node_registrations`.
2. MQTT terminal displays `hardware_id` JSON arrays.
3. Kafka terminal displays the same payload.
4. Consumer shows no processing warning.
5. PostgreSQL reading count increases.

## Success condition

```text
Nesso → BLE gateway → Mosquitto → Kafka Connect → Kafka
      → detection consumer → PostgreSQL
```

Every arrow must have an observed test result, not only a running process.
