# Architecture decisions

## BLE only for provisioning

**Decision:** BLE handles identity/worker/network provisioning; sensor telemetry uses Wi-Fi/MQTT.

**Reasoning:** separates short-range setup from continuous network transport and lets the Nesso communicate directly with server infrastructure after registration.

## Direct MQTT-to-Python ingestion

**Decision:** the current active consumer reads MQTT directly instead of Kafka.

**Trade-off:** simpler deployment and debugging at prototype scale, but less durable centralized stream replay/multi-consumer decoupling than a Kafka-based design.

## Persistent edge store-and-forward

**Decision:** reserve a fixed **512 KiB** region in the Nesso storage partition for offline telemetry.

**Reasoning:** sampling continues even if MQTT is temporarily unavailable.

**Overflow policy:** compact into the alternate bank; with the default policy, purge queued `unknown` batches first, otherwise drop the oldest active batch.

## Hybrid ML + server rules

**Current implementation:** TFLite inference runs on the Nesso and `FallDetector` also runs on every server-received reading.

**Important distinction:** low-confidence ML does not uniquely trigger server rules. Low confidence first invokes the Nesso's local heuristic fallback; server rules remain independent and always process raw readings.

## Application-layer AES-GCM

**Decision:** encrypt telemetry payload content independently of MQTT transport security.

**Consequence:** payload confidentiality/authenticity does not depend on broker TLS, but MQTT transport itself is currently configured as `mqtt://` in firmware, so network metadata and broker authentication/security still require deployment hardening.
