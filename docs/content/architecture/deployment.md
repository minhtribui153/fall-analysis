# Deployment topology

![Deployment topology](../../assets/deployment-topology.svg)

## Active runtime

### Nesso N1
- BLE registration client;
- Wi-Fi station;
- SNTP client;
- model HTTP client;
- BMI270 sensing;
- TFLite Micro inference;
- 512 KiB flash offline journal;
- MQTT publisher.

### Registration/backend host
Runs `detection/n1_registration_server.py` and must provide:
- BLE capability for `TVX-REG`;
- PostgreSQL connectivity;
- TCP port 8765 for admin/model HTTP;
- the HAR model file;
- the registration token secret.

### MQTT/PostgreSQL host
Runs:
- Mosquitto on the configured MQTT port;
- PostgreSQL;
- `detection/mqtt_to_postgres.py`.

These roles may share one Raspberry Pi/server if it has working BLE and enough resources.

## Legacy compose files

`deployment/compose.kafka.yaml`, Kafka Connect configuration and the older MQTT bridge remain in the repository, but `mqtt_to_postgres.py` is the current direct MQTT consumer. Treat Kafka as legacy/alternative infrastructure unless the project intentionally re-enables it.
