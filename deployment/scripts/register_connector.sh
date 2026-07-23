#!/bin/bash
# Registers the MQTT->Kafka bridge with Kafka Connect. Run this once,
# after `make up mqtt-bridge` and once Kafka Connect has finished starting
# (check with: curl http://localhost:8083/ -- should return worker info,
# not a connection error. First boot takes longer since it downloads the
# connector plugin -- check `docker logs sg-nyp-a1-eg2a17-kafka-connect`
# if this script fails with a connection error).

set -e

echo "Registering MQTT source connector..."
curl -s -X POST \
    -H 'Content-Type: application/json' \
    --data @mqtt_source_connector.json \
    http://localhost:8083/connectors

echo ""
echo "Checking connector status..."
curl -s http://localhost:8083/connectors/nesso-sensor-mqtt-source/status
