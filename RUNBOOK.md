# Day-of runbook: first real hardware bring-up

Ordered so each stage only starts once the previous one is actually
confirmed working, not just "looks right." BLE first, always -- nothing
downstream will accept data for a device that hasn't registered.

## 0. Before the device is even involved

- [ ] Apply `deployment/migrations/002_fix_timestamps_and_view.sql`
      against your existing database (pgAdmin Query Tool or
      `psql -d your_database -f 002_fix_timestamps_and_view.sql`).
      Do this before any real readings flow -- it's the fix for the
      crash-on-first-insert bug.
- [ ] Fill in `.env` from `.env.example` (Postgres creds, `DIR_DATA`,
      pgadmin email).
- [ ] `sudo make init` (sets up `.data` permissions), then
      `sudo make deploy up postgres`.
- [ ] Confirm you can connect (pgAdmin or `psql`) and see the
      `devices`, `workers`, `monitoring`, `fall_detection` schemas.

## 1. BLE registration server -- verify standalone, before touching firmware

This is the flakiest layer in the stack (bless/BlueZ/D-Bus on Linux).
Don't debug it through the full firmware boot sequence -- test it in
isolation first.

- [ ] `pip install -r requirements.txt` on the Pi.
- [ ] Run `python n1_registration_server.py`. Watch for it reaching
      "BLE registration server started" with no DBusError.
- [ ] From a phone BLE scanner app (nRF Connect or similar), confirm
      you can see a device advertising as `TVX-REG` with service UUID
      `7db20000-6e8d-4d8d-a991-2cf812340000`.
- [ ] If this step fails, it's a BlueZ/permissions problem on the Pi,
      not a firmware problem -- don't move on until this works.

## 2. Firmware config

- [ ] `cd tervanix-n1 && pio run -e nesso_n1 -t menuconfig`
      (or `pio run -e nesso_n1 -t menuconfig` per env) -- set:
      - Tervanix Wi-Fi → SSID / password
      - Tervanix MQTT → broker URI = **your machine's LAN IP**, not
        `mosquitto` (that hostname only resolves inside the docker
        compose network) -- e.g. `mqtt://192.168.1.50:1883`
- [ ] Apply the patches in `PATCHES.md` if you haven't already.
- [ ] `pio run -t clean` then `pio run -e nesso_n1` -- confirm it
      builds and check the printed flash usage against the 1.5MB
      partition (`board_upload.maximum_size = 1572864`).

## 3. IMU bring-up -- this is where real hardware surprises show up

In `tervanix-n1/src/imu/imu_sensor.cpp`, three things are marked as
unconfirmed placeholders -- resolve them against the actual board
before trusting any reading:

- [ ] **I2C pins**: confirm SDA/SCL against the Nesso N1 schematic
      (currently `GPIO_NUM_21`/`GPIO_NUM_22` as placeholders).
- [ ] **BMI270 I2C address**: `0x68` if SDO is pulled low, `0x69` if
      high -- check the schematic, or run an I2C bus scan if unsure.
- [ ] **Units sanity check**: flash, open the serial monitor, hold the
      board still. `read_imu()`'s accel magnitude should read ~1.0 (1g
      at rest) and gyro near 0. If accel reads ~9.8 or gyro is wildly
      off, the driver is returning SI units (m/s², rad/s) instead of
      g/deg-s -- add the conversion in `read_imu()` (there's a comment
      marking exactly where) before anything downstream can trust the
      thresholds in `detection.py`.

## 4. Kafka Connect bridge

- [ ] `sudo make deploy up mqtt-bridge`.
- [ ] Wait for Kafka Connect to actually finish starting (first boot
      downloads the connector plugin) -- confirm with
      `curl http://localhost:8083/` before registering anything.
- [ ] `cd deployment/scripts && ./register_connector.sh` -- confirm the
      status response shows the connector RUNNING, not FAILED.

## 5. Boot the device and watch the log tags in order

- [ ] Flash (`pio run -e nesso_n1 -t upload`) and open the monitor.
- [ ] `identity` / `ble-register` tags: watch for hardware ID printed,
      then registration written/read, then "New UUID assigned" or
      "Existing UUID accepted" (not "Registration rejected" or
      "Registration failed").
- [ ] `status` tag (every 5s): `BLE=CONNECTED registration=REGISTERED
      stored_uuid=YES` -- if it's still `WAITING` after ~20s, BLE
      registration is timing out; go back to step 1.
- [ ] `wifi` tag: "Connected".
- [ ] `tervanix` tag: "Starting SNTP time sync" → "SNTP sync complete"
      (if this times out, timestamps will be obviously-wrong ~1970
      values -- check the Pi/device actually has internet reachability
      to NTP, not just LAN).
- [ ] `mqtt` tag: "Connected to broker".
- [ ] `imu` tag: "BMI270 ready", no repeated "Accel read failed"
      warnings.

## 6. Confirm data actually lands in Postgres

```sql
-- should show your device registering
SELECT device_id, node_uuid, active, last_seen_at
FROM node_registrations ORDER BY registered_at DESC LIMIT 5;

-- should show recent-looking timestamps (not 1970, not far in the future)
SELECT session_id, ts, ax, ay, az
FROM monitoring.readings ORDER BY ts DESC LIMIT 10;

-- current activity classification, should update every ~5s of movement
SELECT session_id, ts, activity_state
FROM fall_detection.activity_log ORDER BY ts DESC LIMIT 5;
```

If `node_registrations` has a row but `monitoring.readings` stays
empty, check the `kafka_to_postgres.py` process's own stdout for
`[WARN] Failed to process message` -- that's usually a
`device_id`-not-registered mismatch or a Kafka Connect payload-shape
surprise (its own docstring flags the message contract as
"PROPOSED... verify once the connector is actually running").
