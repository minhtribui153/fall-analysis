# Message contracts

## MQTT telemetry topic

`tervanix/sessions/<32-hex-session-uuid>/telemetry`

## Encrypted outer envelope

Conceptually:

```json
{
  "schema_version": 2,
  "alg": "A256GCM",
  "session_id": "<uuid>",
  "worker_id": "<worker-id>",
  "nonce": "<base64>",
  "ciphertext": "<base64>",
  "tag": "<base64>"
}
```

The bearer token itself is not included.

## Decrypted telemetry payload

```json
{
  "session_id": "<uuid>",
  "worker_id": "<worker-id>",
  "device_id": "<device-id>",
  "readings": [
    {
      "sequence": 1,
      "source_uptime_ms": 12345,
      "ts": 1780000000000,
      "ax": 0.0,
      "ay": 0.0,
      "az": 1.0,
      "gx": 0.0,
      "gy": 0.0,
      "gz": 0.0
    }
  ],
  "prediction": {
    "prediction_sequence": 1,
    "source_uptime_ms": 12345,
    "ts": 1780000000000,
    "state": "walking",
    "confidence": 0.91,
    "model_sha256": "<64 hex>",
    "source": "nesso_tflite"
  }
}
```

`prediction` is optional. A stored offline batch can preserve it when present.

## Allowed Nesso prediction states

- `falling_from_height`
- `jumping`
- `near_miss`
- `running`
- `slip`
- `stationary`
- `trip`
- `unknown`
- `walking`
