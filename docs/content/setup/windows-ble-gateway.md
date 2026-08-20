# Registration host notes

This filename is retained for compatibility with existing links, but the role has changed.

The current repository no longer uses a Windows BLE-to-MQTT telemetry gateway. `detection/n1_registration_server.py` is a **BLE registration/model service**. After registration, the Nesso uses Wi-Fi/MQTT directly.

The host running this service must support the Bless BLE backend for its operating system and have access to PostgreSQL plus the HAR model file.

Run:

```bash
python detection/n1_registration_server.py
```

Expected service roles:
- advertise BLE name `TVX-REG`;
- approve/link a Nesso to a worker and session;
- provision credentials/context;
- serve authenticated model manifest and binary over HTTP port 8765.

Do not document this process as a sensor telemetry bridge.
