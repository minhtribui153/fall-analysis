# Troubleshooting

| Symptom | Likely layer | First checks |
|---|---|---|
| `NotPlatformIOProjectError` | Firmware tooling | Run PlatformIO from `tervanix-n1` |
| COM port busy/access denied | USB upload | Close serial monitor, reconnect device, list ports |
| Gateway PostgreSQL timeout | Network/database | Pi container, port 5432, `.env` host |
| MQTT never connects | Network/Mosquitto | Port 1883, broker listener, firewall |
| Nesso keeps scanning | BLE gateway | Gateway started, Bluetooth on, service advertising |
| MTU below 63 | BLE link | Desktop adapter/stack negotiation |
| Telemetry from unregistered device | Registration state | Complete registration after each gateway restart |
| Sequence-gap warnings | BLE throughput/order | Packet loss or callback ordering; do not hide during QA |
| MQTT has data, Kafka does not | Kafka Connect | Connector status, plugin, `mosquitto:1883`, logs |
| Kafka has data, Python warns `device_id`/key missing | Contract mismatch | Change legacy consumer contract to `hardware_id` |
| Consumer cannot reach Kafka | Listener configuration | Run on Pi; port 19092; advertised listener |
| Consumer cannot find registration | Identity/schema mismatch | Same database, active `hardware_id`, column naming |
| First reading insert fails on timestamp | PostgreSQL schema | Use `TIMESTAMPTZ` columns |
| No classification rows | Detector behaviour | Normal activity may not produce event classifications |

## Useful log commands

```bash
docker logs --tail 150 sg-nyp-a1-eg2a17-mosquitto
docker logs --tail 150 sg-nyp-a1-eg2a17-kafka-connect
docker logs --tail 150 sg-nyp-a1-eg2a17-kafka
docker logs --tail 150 sg-nyp-a1-eg2a17-postgres
```

## Fresh Windows environment failures

The tested Windows gateway required compatibility workarounds in Bless/WinRT. When errors occur in `.venv/Lib/site-packages/bless/backends/winrt/server.py`, compare the fresh environment with the previously working virtual environment rather than changing BLE firmware immediately.
