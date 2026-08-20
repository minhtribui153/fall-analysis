# Legacy and generated components

## Legacy architecture files

The repository still contains:
- `deployment/compose.kafka.yaml`;
- `deployment/kafka-connect/`;
- older MQTT bridge configuration;
- older material under `arduino/`.

These files can be useful for historical comparison or alternative architecture testing, but they are not evidence that Kafka or a BLE telemetry gateway is active in the current pipeline.

## Generated/local content

Do not treat these as source documentation:
- `.venv/`
- `.pio/`
- `.cache/`
- build outputs
- local `.env`
- runtime database volumes
- `.git/`
