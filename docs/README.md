# Fall Analysis Documentation

This directory documents the current `fall-analysis` repository as reviewed on **20 August 2026**, after merge commit `23f1bdf` (`compile_fix`).

The active telemetry path is:

`Nesso N1 -> Wi-Fi -> encrypted MQTT -> Mosquitto -> detection/mqtt_to_postgres.py -> PostgreSQL`

BLE is used for registration/provisioning, not for normal sensor streaming. The Nesso also performs embedded HAR inference and contains a persistent flash-backed offline telemetry journal.

> **Important implementation finding:** the current source does **not** route only low-confidence ML predictions to `detection.py`. `har_inference.cpp` applies an on-device heuristic fallback whenever model confidence is below the configured threshold, while `mqtt_to_postgres.py` runs the server-side `FallDetector` independently on every received raw IMU reading.

## Documentation map

### Architecture
- [Overview](content/architecture/overview.md)
- [Components](content/architecture/components.md)
- [Data flow](content/architecture/data-flow.md)
- [Protocols and security](content/architecture/protocols.md)
- [Database](content/architecture/database.md)
- [Deployment](content/architecture/deployment.md)
- [Architecture decisions](content/architecture/decisions.md)
- [Known limitations](content/architecture/known-limitations.md)

### Setup and operations
- [Prerequisites](content/setup/prerequisites.md)
- [Raspberry Pi/server setup](content/setup/raspberry-pi-server.md)
- [Nesso firmware](content/setup/nesso-firmware.md)
- [Registration host notes](content/setup/windows-ble-gateway.md)
- [End-to-end runbook](content/operations/end-to-end-runbook.md)
- [Verification](content/operations/verification.md)
- [Troubleshooting](content/operations/troubleshooting.md)

### Reference
- [Repository map](content/reference/repository-map.md)
- [Configuration](content/reference/configuration.md)
- [Message contracts](content/reference/message-contracts.md)
- [Current implementation verification](content/reference/current-implementation-verification.md)
- [Legacy components](content/reference/legacy-components.md)
- [External references](content/reference/external-references.md)

## System at a glance

![System context](assets/system-context.svg)

## Scope note

The dashboard source is not present inside the uploaded `fall-analysis` repository. These docs therefore document the repository itself. Dashboard integration is described only at the database boundary where the schema exposes readings, activities, classifications and alerts.
