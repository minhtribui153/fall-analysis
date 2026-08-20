# Configuration reference

## Registration service

| Variable | Default / meaning |
|---|---|
| `REGISTRATION_ADMIN_HOST` | `0.0.0.0` |
| `REGISTRATION_ADMIN_PORT` | `8765` |
| `REGISTRATION_ADMIN_TOKEN` | optional admin token |
| `REGISTRATION_SESSION_TTL_SECONDS` | `28800` (8 h) |
| `SAMPLE_RATE_HZ` | `100` |
| `HAR_MODEL_PATH` | `detection/models/har_model.tflite` |
| `HAR_MODEL_ID` | derived from SHA when empty |
| `HAR_MODEL_MIN_CONFIDENCE` | `0.60` |
| `REGISTRATION_TOKEN_SECRET` | shared server secret, min 32 chars |

## MQTT ingestion

| Variable | Default / meaning |
|---|---|
| `MQTT_HOST` | `127.0.0.1` |
| `MQTT_PORT` | `1883` |
| `MQTT_KEEPALIVE` | `60` |
| `MQTT_TELEMETRY_SUBSCRIPTION` | `tervanix/sessions/+/telemetry` |
| `MQTT_QOS` | `0` |
| `MQTT_USERNAME` | optional |
| `MQTT_PASSWORD` | optional |
| `READING_BATCH_SIZE` | `50` |
| `MQTT_QUEUE_SIZE` | `2000` |
| `LAST_SEEN_UPDATE_SECONDS` | `5` |

## Firmware/model defaults

- model sample rate: 25 Hz;
- model window: 50 samples;
- slide: 25 samples;
- features: 8;
- minimum confidence: 0.60;
- acquisition period: 40 ms;
- acquisition history window default: 1500 ms;
- prediction interval default: 1000 ms;
- telemetry batch: 10 samples;
- fixed offline storage reservation: 512 KiB;
- offline default purge policy: unknown-first.

> `.env.example` still contains legacy names/comments and should be refreshed before relying on it as canonical configuration documentation.
