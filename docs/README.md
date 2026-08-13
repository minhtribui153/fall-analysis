# Fall Analysis Documentation

This directory documents the current `fall-analysis` repository as reviewed on **4 August 2026**.

The project collects six-axis BMI270 telemetry from a Nesso N1, transports it through BLE, MQTT and Kafka, classifies activity/fall events, and stores results in PostgreSQL.

> **Canonical identity term:** `hardware_id` is the MAC-derived identifier used to identify a physical Nesso. References to `device_id` in older files are legacy and are listed under [Known limitations](content/architecture/known-limitations.md).

## Documentation map

### Architecture

- [Architecture overview](content/architecture/overview.md)
- [Components](content/architecture/components.md)
- [End-to-end data flow](content/architecture/data-flow.md)
- [BLE and messaging protocols](content/architecture/protocols.md)
- [Deployment topology](content/architecture/deployment.md)
- [Database model](content/architecture/database.md)
- [Architecture decisions](content/architecture/decisions.md)
- [Known limitations and inconsistencies](content/architecture/known-limitations.md)

### Setup

- [Prerequisites](content/setup/prerequisites.md)
- [Raspberry Pi server](content/setup/raspberry-pi-server.md)
- [Windows BLE gateway](content/setup/windows-ble-gateway.md)
- [Nesso N1 firmware](content/setup/nesso-firmware.md)

### Operations

- [End-to-end runbook](content/operations/end-to-end-runbook.md)
- [Verification checklist](content/operations/verification.md)
- [Troubleshooting](content/operations/troubleshooting.md)

### Reference

- [Repository map](content/reference/repository-map.md)
- [Configuration reference](content/reference/configuration.md)
- [Message contracts](content/reference/message-contracts.md)
- [Command cheat sheet](content/reference/command-cheatsheet.md)
- [Legacy and generated components](content/reference/legacy-components.md)
- [External references](content/reference/external-references.md)

## System at a glance

![System context](assets/system-context.svg)

The recommended deployment split is:

- **Nesso N1:** samples the BMI270 and sends binary BLE telemetry.
- **Windows laptop:** hosts the BLE GATT gateway and publishes MQTT messages.
- **Raspberry Pi:** hosts Mosquitto, Kafka Connect, Kafka, PostgreSQL, and the Kafka-to-PostgreSQL Python consumer.

## Documentation scope

These pages document custom project code and configuration. Third-party generated content under `.venv`, `.pio`, `.cache`, `managed_components`, and runtime database directories is intentionally not documented file-by-file.
