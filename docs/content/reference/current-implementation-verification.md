# Current implementation verification

Reviewed against the merged repository on 20 August 2026.

## Claim: "The Nesso now has actual caching"

**Verified: yes.**

Evidence in source:
- `tervanix-n1/include/offline_store.hpp` reserves 512 KiB and defines stored batches;
- `tervanix-n1/src/offline_store.cpp` implements flash journal initialization, enqueue, peek, tombstone acknowledgement and bank compaction;
- `tervanix-n1/src/main.cpp` enqueues failed/unavailable MQTT batches and starts `offline_uploader_task` to replay them;
- the replay record preserves raw samples and optional HAR prediction.

The default full-store compaction policy removes queued `unknown` batches first and otherwise removes the oldest batch.

## Claim: "Low-confidence prediction uses server-side detection.py"

**Not verified as stated. The merged code behaves differently.**

In `tervanix-n1/src/har_inference.cpp`:

```cpp
if (probability < threshold)
{
    state = fallback(max_accel, max_gyro);
}
```

So low-confidence inference is immediately replaced by an on-device heuristic state.

Separately, `detection/mqtt_to_postgres.py` passes every raw IMU reading into the session's `FallDetector`:

```python
result = state.detector.add_sample(ax, ay, az, detector_time, gx, gy, gz)
```

There is no confidence conditional around this call.

### Accurate description of the current design

> The Nesso performs TFLite activity inference. Predictions below the configured confidence threshold are replaced by an on-device magnitude-based heuristic classification. Independently, the backend processes every received raw IMU reading through `detection.py` and stores server-rule outcomes separately from Nesso predictions.

If the intended architecture is specifically `confidence < 0.60 -> defer classification to detection.py`, the current merged code still needs a behaviour change.
