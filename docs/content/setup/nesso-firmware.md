# Nesso N1 firmware

## Build location

```bash
cd tervanix-n1
pio run -e nesso_n1
```

Use the existing PlatformIO environment and board configuration in `platformio.ini`.

## Runtime prerequisites

The device requires a successful BLE registration before enabling Wi-Fi telemetry. Registration supplies the session/worker context and credentials required for model and MQTT authentication.

## Runtime sequence

1. initialize NVS and identity;
2. BLE registration with `TVX-REG`;
3. Wi-Fi connection;
4. SNTP time synchronization;
5. authenticated model manifest/binary verification;
6. MQTT startup;
7. offline flash journal initialization;
8. BMI270 initialization;
9. sensor + replay tasks.

## Offline telemetry

The firmware reserves 512 KiB in the `storage` partition for a two-bank journal. A failed batch publish is enqueued in flash and replayed when MQTT reconnects.

## Health output

The status task logs BLE, registration, Wi-Fi, time, MQTT/auth, model, state and offline queue usage (`OFFLINE=queued/capacity`).
