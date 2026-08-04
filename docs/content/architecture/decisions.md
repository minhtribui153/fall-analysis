# Architecture decisions

## BLE-only Nesso

**Decision:** remove Wi-Fi and MQTT from the active embedded build.

**Reasoning:** the Nesso performs a smaller, deterministic workload: sample, batch and transmit. Network protocols and JSON serialization are moved to a more capable gateway.

**Consequence:** a gateway computer is mandatory; BLE cannot directly publish normal MQTT/TCP traffic.

## Nesso acts as BLE central

**Decision:** the Nesso scans for and connects to a GATT server named `TVX-REG`.

**Reasoning:** this preserves the registration architecture already implemented in the project.

**Consequence:** the laptop/Pi Bluetooth stack must support acting as a BLE peripheral/GATT server, which is less consistently supported than central mode on desktop operating systems.

## Gateway owns timestamps

**Decision:** timestamp readings when the gateway receives BLE packets.

**Reasoning:** the Nesso no longer has Wi-Fi/SNTP. The known 100 Hz period allows two samples in a packet to be spaced by 10 ms.

**Consequence:** timestamps include gateway receipt latency and do not recover the exact original capture time during packet delay or reordering.

## Two samples per BLE packet

**Decision:** pack two six-axis float samples into 60 bytes.

**Reasoning:** the packet fits within the negotiated 64-byte ATT MTU without custom fragmentation.

**Consequence:** approximately 50 BLE writes per second are required at 100 Hz.

## Write Without Response

**Decision:** use unacknowledged telemetry writes.

**Reasoning:** reduces round-trip overhead at a high write rate.

**Consequence:** packets may be dropped or observed out of order; the sequence number detects but does not repair gaps.

## Ten readings per MQTT message

**Decision:** the gateway re-batches BLE samples into arrays of ten readings.

**Reasoning:** reduces broker and Kafka Connect overhead while preserving an approximately 10-message-per-second stream.

## Threshold-based detection

**Decision:** use transparent signal thresholds rather than an embedded or server ML model in the active pipeline.

**Reasoning:** thresholds are easier to explain, recalibrate and debug with the current dataset and timeline.

**Consequence:** thresholds are dataset- and mounting-dependent and require further validation across people and realistic events.
