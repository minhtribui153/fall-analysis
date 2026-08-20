# End-to-end runbook

## A. Start PostgreSQL and load schema

Use `detection/schemas/database_schema.sql` and verify all three schemas (`identification`, `monitoring`, `fall_detection`) exist.

## B. Start MQTT broker

Confirm the Nesso can reach the configured broker IP/port.

## C. Start registration/model service

```bash
python detection/n1_registration_server.py
```

Check `/health` on port 8765.

## D. Start ingestion

```bash
python detection/mqtt_to_postgres.py
```

Expected startup includes a successful PostgreSQL connection and MQTT subscription.

## E. Register/approve Nesso

1. power/reset the Nesso;
2. confirm BLE registration request appears;
3. approve/link to a worker using the admin workflow;
4. verify a monitoring session is created;
5. allow Nesso to connect Wi-Fi, sync time and verify/download model.

## F. Validate live telemetry

Verify in order:
1. Nesso reports MQTT authenticated;
2. `monitoring.readings` increases;
3. `fall_detection.activity_log` receives Nesso/server activity changes;
4. `fall_detection.classification` receives staged events when triggered.

## G. Validate offline replay

1. establish live telemetry;
2. disconnect or stop MQTT while leaving Nesso sampling;
3. confirm `OFFLINE=...` queue count increases on the Nesso;
4. restore MQTT;
5. confirm the offline count decreases and PostgreSQL receives the preserved sequence/timestamp records.

## H. Validate low-confidence behaviour

Current expected source behaviour is **local heuristic fallback plus independent server rules**, not conditional server routing. Capture the Nesso log state/confidence and compare `nesso_tflite` and `server_rules` rows in PostgreSQL.
