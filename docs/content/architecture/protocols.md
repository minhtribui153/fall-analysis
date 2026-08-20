# Protocols and security

## BLE registration

Service name: `TVX-REG`

| Item | UUID |
|---|---|
| Service | `7db20000-6e8d-4d8d-a991-2cf812340000` |
| Registration | `7db20001-6e8d-4d8d-a991-2cf812340000` |
| Credential | `7db20002-6e8d-4d8d-a991-2cf812340000` |
| Worker context | `7db20003-6e8d-4d8d-a991-2cf812340000` |
| Worker name | `7db20004-6e8d-4d8d-a991-2cf812340000` |

BLE is used for registration/provisioning. Normal IMU telemetry uses Wi-Fi/MQTT.

## HTTP model API

- `GET /health`
- `GET /v1/device/model/manifest`
- `GET /v1/device/model/binary`

Model requests require session/device headers plus an HMAC request signature. The manifest itself is canonicalized and HMAC signed; the binary is verified by SHA-256.

## MQTT topics

The firmware builds session-scoped topics. Key suffixes are:
- `telemetry`
- `state`
- `auth/status`

Telemetry and state are published by the Nesso with **QoS 1**. The Python subscriber's requested QoS is configurable by `MQTT_QOS` and currently defaults to **0**, so do not describe the complete Nesso-to-Python path as end-to-end QoS 1 unless `MQTT_QOS=1` is explicitly configured.

## Application-layer encryption

MQTT telemetry uses AES-256-GCM. The Python backend reconstructs the session token, verifies its SHA-256 hash, derives the AES key as `SHA256(token)`, and decrypts only an active/unexpired session.

## Replay and duplicates

Offline replay and MQTT QoS behaviour make duplicate handling important. Raw reading insertion is idempotent through the unique `(session_id, sequence)` constraint and `ON CONFLICT DO NOTHING`.
