# Windows BLE gateway setup

## 1. Use the existing working environment

The tested environment contains local Bless/WinRT compatibility changes. Rebuilding it from `requirements.txt` alone may not reproduce the same behaviour.

Activate:

```powershell
cd C:\Users\<user>\Documents\fall-analysis
.\.venv\Scripts\Activate.ps1
```

## 2. Point `.env` at the Pi

```env
POSTGRES_DB="fall_analysis"
POSTGRES_USER="eg2a17"
POSTGRES_PASSWORD="..."
POSTGRES_HOST="<PI_IP>"
POSTGRES_PORT="5432"

MQTT_HOST="<PI_IP>"
MQTT_PORT="1883"
MQTT_TOPIC="sensor-readings"
MQTT_CLIENT_ID="tervanix-ble-gateway"
```

## 3. Verify network reachability

```powershell
Test-NetConnection <PI_IP> -Port 5432
Test-NetConnection <PI_IP> -Port 1883
```

Both must return `TcpTestSucceeded : True`.

## 4. Start the gateway

```powershell
python n1_registration_server.py
```

Expected milestones:

```text
PostgreSQL schema ready
Starting MQTT bridge to <PI_IP>:1883
MQTT connected to <PI_IP>:1883; topic=sensor-readings
Advertising name: TVX-REG
BLE registration + telemetry gateway started
```

## 5. Keep the terminal open

The process must remain running while the Nesso streams. Stopping it removes the BLE peripheral and flushes any partial MQTT batch.

## Fresh-environment warning

The uploaded source still contains `writeable` rather than the tested Bless API's `writable`, and the WinRT advertising overload was patched inside the virtual environment during testing. See [Known limitations](../architecture/known-limitations.md).
