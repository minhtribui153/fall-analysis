# Raspberry Pi / server setup

This page describes the active direct-MQTT architecture rather than the older Kafka path.

## 1. PostgreSQL

Create the configured PostgreSQL database and load:

```bash
psql "$DATABASE_URL" -f detection/schemas/database_schema.sql
```

Alternatively provide `POSTGRES_HOST`, `POSTGRES_PORT`, `POSTGRES_DB`, `POSTGRES_USER`, and `POSTGRES_PASSWORD`.

## 2. Mosquitto

Run a broker reachable by the Nesso. Development deployments may use port 1883, but production/untrusted networks should add proper broker authentication and transport security.

## 3. Python environment

```bash
python3 -m venv .venv
source .venv/bin/activate
pip install -r detection/requirements.txt
```

## 4. Registration/model service

Ensure the HAR model exists at the configured `HAR_MODEL_PATH` and configure the registration token secret.

```bash
python detection/n1_registration_server.py
```

Default HTTP/admin port: `8765`.

## 5. MQTT ingestion

```bash
python detection/mqtt_to_postgres.py
```

Default subscription: `tervanix/sessions/+/telemetry`.

Set `MQTT_QOS=1` if broker-to-consumer QoS 1 is required; the current code default is 0.

## 6. Legacy services

Kafka and Kafka Connect are not required by `mqtt_to_postgres.py` and should not be started unless deliberately testing the older architecture.
