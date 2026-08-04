# Raspberry Pi server setup

## 1. Prepare `.env`

From the repository root:

```bash
cp .env.example .env
nano .env
```

Set database credentials and `DIR_DATA`. Pi-hosted processes should normally use `POSTGRES_HOST=localhost` and Kafka broker `localhost:19092`.

## 2. Prepare runtime directories

```bash
sudo make init
mkdir -p "${DIR_DATA:-./.data}/instances/mosquitto/config"
mkdir -p "${DIR_DATA:-./.data}/instances/mosquitto/data"
mkdir -p "${DIR_DATA:-./.data}/instances/mosquitto/log"
```

Example test-only Mosquitto configuration:

```conf
listener 1883
allow_anonymous true
log_dest stdout
```

Do not expose anonymous MQTT to an untrusted network.

## 3. Start PostgreSQL

```bash
sudo make deploy up postgres
```

Check:

```bash
docker exec sg-nyp-a1-eg2a17-postgres \
  pg_isready -U "$POSTGRES_USER" -d "$POSTGRES_DB"
```

Load the team's base schema before applying later migrations.

## 4. Start Kafka

```bash
sudo make deploy up kafka
```

Create the topic if needed:

```bash
docker exec sg-nyp-a1-eg2a17-kafka \
  /opt/kafka/bin/kafka-topics.sh \
  --bootstrap-server localhost:9092 \
  --create --if-not-exists \
  --topic sensor-readings \
  --partitions 1 --replication-factor 1
```

## 5. Start Mosquitto and Kafka Connect

```bash
sudo make deploy up mqtt-bridge
```

Wait for Connect:

```bash
curl http://localhost:8083/
curl http://localhost:8083/connector-plugins
```

The plugin list must include `io.confluent.connect.mqtt.MqttSourceConnector`.

## 6. Register the connector

```bash
curl -X POST \
  -H 'Content-Type: application/json' \
  --data @deployment/kafka-connect/mqtt_source_connector.json \
  http://localhost:8083/connectors
```

Check status:

```bash
curl http://localhost:8083/connectors/nesso-sensor-mqtt-source/status
```

Both connector and task must be `RUNNING`.

## 7. Start the Kafka-to-PostgreSQL process

Create a Pi-specific environment without desktop BLE dependencies:

```bash
python3 -m venv .venv-pipeline
source .venv-pipeline/bin/activate
python -m pip install asyncpg==0.31.0 aiokafka==0.14.0 python-dotenv==1.2.2
python detection/kafka_to_postgres.py
```

> Reconcile the consumer's legacy `device_id` references with `hardware_id` before a live updated-schema run.
