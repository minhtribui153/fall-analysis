# Legacy and generated components

## Legacy firmware networking

`wifi_manager.cpp/.hpp` and `mqtt_publisher.cpp/.hpp` belong to the earlier Nesso Wi-Fi → MQTT architecture. They are not included in the active `tervanix-n1/src/CMakeLists.txt` source list.

## Arduino prototypes

### `arduino/sensor-telemetry.ino`

Early serial CSV accelerometer capture using `Arduino_BMI270_BMM150`.

### `arduino/prediction_Fall_analysis`

TensorFlow Lite Micro inference prototype. It currently fills the model input with dummy values and is not connected to the active pipeline.

### `arduino/model.h`

Generated model byte array used by the TFLite prototype.

### `BLE_template`

Earlier BLE-peripheral/notification example using Arduino libraries. It uses unrelated UUIDs and the opposite BLE role from the active ESP-IDF firmware.

## Managed components

`tervanix-n1/managed_components` contains Espressif dependencies managed by the ESP-IDF component manager. Their upstream documentation should be used rather than adding project-specific docs to every file.

## Generated/runtime content

- `.venv`: local Python environment;
- `.pio`: PlatformIO builds and packages;
- `.cache`: clangd index;
- `deployment/.data`: live PostgreSQL, pgAdmin, Kafka and plugin data;
- `.git`: repository metadata.

These are excluded from the cleaned documented-source archive.
