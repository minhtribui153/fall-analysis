# Protocols and data contracts

## BLE service

| Purpose | UUID | Properties |
|---|---|---|
| Registration service | `7db20000-6e8d-4d8d-a991-2cf812340000` | Primary service |
| Registration characteristic | `7db20001-6e8d-4d8d-a991-2cf812340000` | Read, write |
| Telemetry characteristic | `7db20002-6e8d-4d8d-a991-2cf812340000` | Write, write without response |

## Registration request

C++ definition: `tervanix-n1/include/registration_protocol.hpp`  
Python format: `<BB6s16sI`

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | version | Protocol version, currently `1` |
| 1 | 1 | flags | Bit 0 indicates a stored UUID is present |
| 2 | 6 | hardware_id | Raw MAC-derived bytes |
| 8 | 16 | current_uuid | Previously assigned UUID or all zeros |
| 24 | 4 | nonce | Little-endian request nonce |

Total: **28 bytes**.

## Registration response

Python format: `<B16sI`

| Offset | Size | Field | Meaning |
|---:|---:|---|---|
| 0 | 1 | status | 0 error, 1 existing accepted, 2 new assigned |
| 1 | 16 | assigned_uuid | Accepted or newly assigned UUID |
| 17 | 4 | nonce | Must match request nonce |

Total: **21 bytes**.

## Telemetry packet

C++ definition: `tervanix-n1/include/telemetry_protocol.hpp`  
Python header: `<BB6sI`  
Python sample: `<ffffff`

| Offset | Size | Field |
|---:|---:|---|
| 0 | 1 | version |
| 1 | 1 | sample_count, 1–2 |
| 2 | 6 | hardware_id |
| 8 | 4 | first_sample_sequence |
| 12 | 24 | sample 1: ax, ay, az, gx, gy, gz |
| 36 | 24 | sample 2: ax, ay, az, gx, gy, gz |

Total: **60 bytes**. With an ATT MTU of 64, a write has 61 payload bytes after the three-byte ATT header, so the packet fits without application-level fragmentation.

## MQTT contract

Topic: `sensor-readings`  
QoS: 1  
Retained: false

The gateway publishes an array of ten readings:

```json
[
  {
    "hardware_id": "48:F6:EE:C4:CB:E8",
    "ts": 1785221234567,
    "ax": 0.01,
    "ay": -0.02,
    "az": 0.99,
    "gx": 1.2,
    "gy": 0.5,
    "gz": -0.3
  }
]
```

## Kafka contract

Kafka topic: `sensor-readings`.

Kafka Connect uses byte-array converters, so the intended consumer input is the raw MQTT UTF-8 JSON payload. The consumer accepts both an array and a single object.

## Identity terminology

- `hardware_id`: stable physical identifier derived from the ESP32 station MAC.
- `node_uuid`: UUID assigned during BLE registration and persisted in NVS.
- `session_id`: PostgreSQL monitoring session identifier.

`hardware_id` is the canonical name in the updated architecture. `device_id` is a legacy field name still present in several repository files.
