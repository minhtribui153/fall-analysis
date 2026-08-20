# Troubleshooting

| Symptom | Likely layer | First checks |
|---|---|---|
| Nesso stays at registration | BLE | `TVX-REG` service, approval state, BLE backend |
| Wi-Fi never starts | registration/context | registration completed and credentials stored |
| Model remains unavailable | HTTP/model | port 8765, `HAR_MODEL_PATH`, session signature, SHA/HMAC logs |
| MQTT auth waiting/rejected | session/security | active session, token lifetime, registration secret consistency |
| Raw readings stop during MQTT outage | firmware/offline | sensor task, `OFFLINE=queued/capacity`, flash init logs |
| Offline queue grows but does not drain | MQTT/replay | broker connectivity, auth rejection, replay publish errors |
| Offline bank compacts | capacity | expected when fixed journal fills; inspect purge counters |
| PostgreSQL has no data | ingestion | MQTT subscription, decryption/auth warnings, DB config |
| Duplicate packets observed | MQTT/replay | expected possibility; verify `(session_id, sequence)` remains unique |
| Claim says low confidence invokes server fallback only | logic mismatch | inspect `har_inference.cpp::fallback()` and `mqtt_to_postgres.py` detector loop |
| Nesso predictions differ from server rules | dual-detector design | compare `source`, timestamps and physical rule evidence |
| Kafka services fail | legacy path | active ingestion does not require Kafka |

## High-value logs

Nesso:
- `Offline store ready...`
- `OFFLINE=queued/capacity`
- `prediction #... <state> <confidence>`
- `Offline replay ack failed...`

Python:
- `[AUTH] Encrypted MQTT session accepted...`
- MQTT connect/subscription line
- decrypt/process warnings
