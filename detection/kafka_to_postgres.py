"""
kafka_to_postgres.py

Subscribes to the sensor-readings Kafka topic (the far side of the MQTT
-> Kafka Connect bridge from the ESP32 nodes), runs each reading through
the same FallDetector/ActivityClassifier already validated against real
hardware data, and writes everything into Postgres using the
devices/workers/monitoring/fall_detection schema.

Expected Kafka message (JSON, one reading per message):
{
    "device_id": "AA:BB:CC:DD:EE:FF",   # matches devices.identification.device_id
    "ts": 1721638445123,                 # epoch milliseconds
    "ax": 0.03, "ay": -0.18, "az": 0.99,
    "gx": 1.2,  "gy": -0.5,  "gz": 0.1
}

This is a PROPOSED contract -- once the Kafka Connect MQTT Source
Connector is actually running, check what it really produces (it may
wrap the raw MQTT payload with extra metadata fields depending on how
the connector's configured) and adjust `parse_message()` below to match.

BLE registration comes first: a device_id only gets a row (and a
node_uuid) here once n1_registration_server.py has already registered it
over BLE -- see get_or_create_session() below. This consumer doesn't mint
its own UUIDs anymore; it looks up the one node_registrations already
assigned, so a physical device maps to exactly one UUID across both
halves of the pipeline.

Run:
    python kafka_to_postgres.py
Config via environment variables (see the block below for defaults).
"""

import asyncio
import json
import os
from datetime import datetime, timezone

import asyncpg
from aiokafka import AIOKafkaConsumer
from dotenv import load_dotenv

from detection import FallDetector

load_dotenv()

# ── Config -- override via environment variables ────────────────────────
# Defaults match the team's compose.kafka.yaml / compose.postgres.yaml as
# deployed. KAFKA_BROKER uses port 19092 (the EXTERNAL listener) since this
# script runs on the host machine, not inside the kafka container's own
# Docker network -- port 9092 (INTERNAL) is only reachable from other
# containers on that same network.
KAFKA_BROKER   = os.environ.get("KAFKA_BROKER", "localhost:19092")
KAFKA_TOPIC    = os.environ.get("KAFKA_TOPIC", "sensor-readings")
KAFKA_GROUP_ID = os.environ.get("KAFKA_GROUP_ID", "postgres-writer")

# Reuses the SAME POSTGRES_* env vars (from the project .env, see
# .env.example) as n1_registration_server.py, instead of a separate
# PG*-prefixed set with its own different-looking defaults. This consumer
# reads rows from node_registrations that the registration server wrote,
# so the two have to be pointed at the same database -- if they read
# different env vars with different defaults, this could silently connect
# to the wrong database and never find any registrations. POSTGRES_DB/
# USER/PASSWORD are required (no silent fallback); DATABASE_URL can be
# set instead if you'd rather pass a full DSN.
def make_db_config() -> dict:
    database_url = os.environ.get("DATABASE_URL")
    if database_url:
        return {"dsn": database_url}

    missing = [
        name
        for name in ("POSTGRES_DB", "POSTGRES_USER", "POSTGRES_PASSWORD")
        if not os.environ.get(name)
    ]
    if missing:
        raise RuntimeError(
            "Set DATABASE_URL or define: " + ", ".join(missing)
        )

    return dict(
        host=os.environ.get("POSTGRES_HOST", "localhost"),
        port=int(os.environ.get("POSTGRES_PORT", 5432)),
        database=os.environ["POSTGRES_DB"],
        user=os.environ["POSTGRES_USER"],
        password=os.environ["POSTGRES_PASSWORD"],
    )


DB_CONFIG = make_db_config()

SAMPLE_RATE = 100     # must match whatever the firmware actually samples at
READING_BATCH_SIZE = 50   # buffer this many raw readings before flushing to Postgres


class DeviceState:
    """Everything the pipeline needs to keep per-device between messages:
    which open session it's writing into, its own FallDetector (state
    persists across samples so the rolling buffer/cooldown/etc. work
    correctly), a small write buffer for batching raw readings, and the
    last activity state actually written (so activity_log only grows on
    a genuine change, not every single 10ms sample)."""

    def __init__(self, session_id):
        self.session_id = session_id
        self.detector = FallDetector(sample_rate=SAMPLE_RATE)
        self.reading_buffer = []
        self.last_logged_activity = None


class Pipeline:
    def __init__(self):
        self.pool = None
        self.device_states = {}

    async def connect(self):
        self.pool = await asyncpg.create_pool(**DB_CONFIG)

    async def close(self):
        # flush anything still sitting in per-device buffers before exiting
        for state in self.device_states.values():
            await self._flush_readings(state)
        await self.pool.close()

    async def get_or_create_session(self, device_id):
        async with self.pool.acquire() as conn:
            # node_registrations is owned by the BLE registration server
            # (n1_registration_server.py) -- a device has to complete BLE
            # registration there before it ever shows up here. We look up
            # the node_uuid it was assigned over BLE rather than minting a
            # new one on this side, so the same physical device maps to
            # the same UUID everywhere. If there's no active registration
            # yet, that's a real problem worth surfacing rather than
            # quietly inventing an identity for a device nobody's
            # confirmed the hardware for.
            registration = await conn.fetchrow(
                """SELECT node_uuid FROM node_registrations
                   WHERE device_id = $1 AND active = TRUE""",
                device_id,
            )
            if registration is None:
                raise RuntimeError(
                    f"Device {device_id} has no active BLE registration in "
                    f"node_registrations -- it needs to register via BLE "
                    f"before its readings can be logged"
                )
            node_uuid = registration["node_uuid"]

            # register the device_id/node_uuid pairing here too, if this is
            # the first time the Kafka side has seen it
            await conn.execute(
                """INSERT INTO devices.identification (device_id, node_uuid)
                   VALUES ($1, $2)
                   ON CONFLICT (device_id) DO NOTHING""",
                device_id, node_uuid,
            )

            # reuse an already-open session for this device if one exists,
            # otherwise start a new one
            row = await conn.fetchrow(
                """SELECT session_id FROM monitoring.sessions
                   WHERE device_id = $1 AND end_time IS NULL
                   ORDER BY start_time DESC LIMIT 1""",
                device_id,
            )
            if row:
                return row["session_id"]

            row = await conn.fetchrow(
                """INSERT INTO monitoring.sessions (device_id, sample_rate_hz)
                   VALUES ($1, $2) RETURNING session_id""",
                device_id, SAMPLE_RATE,
            )
            return row["session_id"]

    async def _flush_readings(self, state):
        if not state.reading_buffer:
            return
        rows = state.reading_buffer
        state.reading_buffer = []
        async with self.pool.acquire() as conn:
            await conn.executemany(
                """INSERT INTO monitoring.readings
                   (session_id, ts, ax, ay, az, gx, gy, gz, smv, gyro_magnitude)
                   VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)""",
                rows,
            )

    async def handle_reading(self, msg):
        device_id = msg["device_id"]
        ts = datetime.fromtimestamp(msg["ts"] / 1000.0, tz=timezone.utc)
        ax, ay, az = float(msg["ax"]), float(msg["ay"]), float(msg["az"])
        gx = float(msg.get("gx", 0.0))
        gy = float(msg.get("gy", 0.0))
        gz = float(msg.get("gz", 0.0))

        if device_id not in self.device_states:
            session_id = await self.get_or_create_session(device_id)
            self.device_states[device_id] = DeviceState(session_id)
        state = self.device_states[device_id]

        smv = (ax ** 2 + ay ** 2 + az ** 2) ** 0.5
        gyro_mag = (gx ** 2 + gy ** 2 + gz ** 2) ** 0.5

        # buffer the raw reading, flush in batches rather than one
        # INSERT per sample -- same reasoning as the BLE capture scripts:
        # at 100Hz, per-sample inserts would mean 100 round-trips/sec to
        # Postgres for every single device
        state.reading_buffer.append(
            (state.session_id, ts, ax, ay, az, gx, gy, gz, smv, gyro_mag)
        )
        if len(state.reading_buffer) >= READING_BATCH_SIZE:
            await self._flush_readings(state)

        # keep last_seen_at honest -- this is what that column is for
        # (shared with node_registrations.last_seen_at, which the BLE
        # registration server updates on its own side)
        async with self.pool.acquire() as conn:
            await conn.execute(
                "UPDATE devices.identification SET last_seen_at = NOW() WHERE device_id = $1",
                device_id,
            )

        # run it through the same validated detector used on real hardware data
        result = state.detector.add_sample(ax, ay, az, ts.timestamp(), gx, gy, gz)
        if result:
            async with self.pool.acquire() as conn:
                await conn.execute(
                    """INSERT INTO fall_detection.classification
                       (session_id, ts, event_type, fall_type, max_smv, max_rotation,
                        freefall_duration, tilt_change, responsiveness, activity_before_event)
                       VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)""",
                    state.session_id, ts, result["event_type"], result["fall_type"],
                    result["max_smv"], result["max_rotation"], result["freefall_duration"],
                    result["tilt_change"], result["responsiveness"],
                    result["activity_before_event"],
                )

        # activity_log only grows on a genuine change of state, not every sample
        current_activity = state.detector.current_activity
        if current_activity and current_activity != "unknown" and current_activity != state.last_logged_activity:
            state.last_logged_activity = current_activity
            async with self.pool.acquire() as conn:
                await conn.execute(
                    """INSERT INTO fall_detection.activity_log (session_id, ts, activity_state)
                       VALUES ($1,$2,$3)""",
                    state.session_id, ts, current_activity,
                )


def parse_message(raw_value: bytes):
    """Deserializes one Kafka message. Update this if the Kafka Connect
    MQTT Source Connector wraps the payload differently than a plain
    passthrough of what the ESP32 published -- worth checking once
    that connector is actually running."""
    return json.loads(raw_value.decode("utf-8"))


async def main():
    pipeline = Pipeline()
    await pipeline.connect()
    print(f"Connected to Postgres ({'DSN' if 'dsn' in DB_CONFIG else DB_CONFIG.get('database')})")

    consumer = AIOKafkaConsumer(
        KAFKA_TOPIC,
        bootstrap_servers=KAFKA_BROKER,
        group_id=KAFKA_GROUP_ID,
        value_deserializer=parse_message,
        auto_offset_reset="latest",
    )
    await consumer.start()
    print(f"Listening on Kafka topic '{KAFKA_TOPIC}' at {KAFKA_BROKER}... Ctrl+C to stop.")

    try:
        async for msg in consumer:
            try:
                # Firmware publishes batches (a JSON array of readings per
                # message) to cut down on MQTT/network overhead -- handle
                # both that and a single-reading object, in case the Kafka
                # Connect bridge or firmware config changes later.
                payload = msg.value
                readings = payload if isinstance(payload, list) else [payload]
                for reading in readings:
                    await pipeline.handle_reading(reading)
            except Exception as e:
                print(f"[WARN] Failed to process message: {e}")
    except KeyboardInterrupt:
        pass
    finally:
        await consumer.stop()
        await pipeline.close()


if __name__ == "__main__":
    asyncio.run(main())
