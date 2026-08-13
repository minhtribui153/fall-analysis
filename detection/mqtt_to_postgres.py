"""Consume ESP32 MQTT telemetry and persist it using the supplied schema.

Data paths are intentionally separate:
    ESP32 -> BLE -> n1_registration_server.py -> registration tables
    ESP32 -> Wi-Fi -> MQTT -> this script -> monitoring/fall-detection tables

MQTT topic:
    tervanix/devices/<12-hex-device-id>/telemetry

Each payload is an authenticated envelope:
    {
      "schema_version": 1,
      "device_id": "AA:BB:CC:DD:EE:FF",
      "bearer_token": "...",
      "readings": [{...}, ...]
    }

The bearer token is validated against public.node_registrations.token_hash before
any sample is written. node_uuid remains BLE-registration-only.
"""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import os
import time
from datetime import datetime, timezone
from typing import Any

import asyncpg
import paho.mqtt.client as mqtt
from dotenv import load_dotenv

from detection import FallDetector

load_dotenv()

MQTT_HOST = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_KEEPALIVE = int(os.getenv("MQTT_KEEPALIVE", "60"))
MQTT_TOPIC = os.getenv(
    "MQTT_TELEMETRY_SUBSCRIPTION",
    "tervanix/devices/+/telemetry",
)
MQTT_QOS = int(os.getenv("MQTT_QOS", "0"))
MQTT_USERNAME = os.getenv("MQTT_USERNAME", "").strip()
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD", "")

SAMPLE_RATE = int(os.getenv("SAMPLE_RATE_HZ", "100"))
READING_BATCH_SIZE = int(os.getenv("READING_BATCH_SIZE", "50"))
MQTT_QUEUE_SIZE = int(os.getenv("MQTT_QUEUE_SIZE", "2000"))
LAST_SEEN_UPDATE_SECONDS = float(os.getenv("LAST_SEEN_UPDATE_SECONDS", "5"))


def make_db_config() -> dict[str, Any]:
    database_url = os.getenv("DATABASE_URL")
    if database_url:
        return {"dsn": database_url}

    missing = [
        name
        for name in ("POSTGRES_DB", "POSTGRES_USER", "POSTGRES_PASSWORD")
        if not os.getenv(name)
    ]
    if missing:
        raise RuntimeError(
            "Set DATABASE_URL or define: " + ", ".join(missing)
        )

    return {
        "host": os.getenv("POSTGRES_HOST", "localhost"),
        "port": int(os.getenv("POSTGRES_PORT", "5432")),
        "database": os.environ["POSTGRES_DB"],
        "user": os.environ["POSTGRES_USER"],
        "password": os.environ["POSTGRES_PASSWORD"],
    }


DB_CONFIG = make_db_config()


def normalize_device_id(value: str) -> str:
    parts = value.strip().upper().split(":")
    if len(parts) != 6 or any(len(part) != 2 for part in parts):
        raise ValueError("device_id must use AA:BB:CC:DD:EE:FF format")
    try:
        bytes(int(part, 16) for part in parts)
    except ValueError as error:
        raise ValueError("device_id contains non-hexadecimal bytes") from error
    return ":".join(parts)



class DeviceState:
    def __init__(self, session_id: int) -> None:
        self.session_id = session_id
        self.detector = FallDetector(sample_rate=SAMPLE_RATE)
        self.reading_buffer: list[tuple[Any, ...]] = []
        self.last_logged_activity: str | None = None
        self.last_seen_update = 0.0
        self.last_sequence: int | None = None


class Pipeline:
    def __init__(self) -> None:
        self.pool: asyncpg.Pool | None = None
        self.device_states: dict[str, DeviceState] = {}
        self.warned_unregistered_devices: set[str] = set()
        self.auth_state: dict[str, bool] = {}
        self.auth_failure_reason: dict[str, str] = {}
        self.last_auth_db_update: dict[str, float] = {}

    async def connect(self) -> None:
        self.pool = await asyncpg.create_pool(**DB_CONFIG)
        await self._validate_schema()

    async def close(self) -> None:
        if self.pool is None:
            return
        for state in self.device_states.values():
            await self._flush_readings(state)
        await self.pool.close()
        self.pool = None

    def _require_pool(self) -> asyncpg.Pool:
        if self.pool is None:
            raise RuntimeError("Postgres pool is not connected")
        return self.pool

    async def _validate_schema(self) -> None:
        """Fail fast when PostgreSQL is still using the pre-device_id schema."""
        pool = self._require_pool()

        required_columns: dict[tuple[str, str], set[str]] = {
            ("public", "node_registrations"): {
                "registration_id",
                "device_id",
                "node_uuid",
                "approval_status",
                "token_hash",
                "token_issued_at",
                "token_revoked_at",
                "active",
                "registered_at",
                "last_seen_at",
                "last_authenticated_at",
                "last_auth_failure_at",
                "delinked_at",
            },
            ("devices", "identification"): {
                "device_id",
                "ble_address",
                "registered_at",
                "last_seen_at",
            },
            ("monitoring", "sessions"): {
                "session_id",
                "device_id",
                "worker_id",
                "start_time",
                "end_time",
                "sample_rate_hz",
            },
            ("monitoring", "readings"): {
                "reading_id",
                "session_id",
                "ts",
                "ax",
                "ay",
                "az",
                "gx",
                "gy",
                "gz",
                "smv",
                "gyro_magnitude",
            },
            ("fall_detection", "classification"): {
                "classification_id",
                "session_id",
                "ts",
                "event_type",
            },
            ("fall_detection", "activity_log"): {
                "activity_id",
                "session_id",
                "ts",
                "activity_state",
            },
        }

        expected_session_fks = {
            ("monitoring", "readings"),
            ("fall_detection", "classification"),
            ("fall_detection", "activity_log"),
        }

        async with pool.acquire() as connection:
            for (schema_name, table_name), expected in required_columns.items():
                rows = await connection.fetch(
                    """
                    SELECT column_name
                    FROM information_schema.columns
                    WHERE table_schema = $1
                      AND table_name = $2
                    """,
                    schema_name,
                    table_name,
                )
                actual = {str(row["column_name"]) for row in rows}
                missing = expected - actual
                if missing:
                    raise RuntimeError(
                        f"Database schema mismatch for {schema_name}.{table_name}: "
                        f"missing columns {sorted(missing)}. Apply the current schema "
                        "before starting mqtt_to_postgres.py."
                    )

            # devices.identification.node_uuid belonged to the old schema and
            # must not be present in the current device_id-only model.
            obsolete_node_uuid = await connection.fetchval(
                """
                SELECT EXISTS (
                    SELECT 1
                    FROM information_schema.columns
                    WHERE table_schema = 'devices'
                      AND table_name = 'identification'
                      AND column_name = 'node_uuid'
                )
                """
            )
            if obsolete_node_uuid:
                raise RuntimeError(
                    "Old schema detected: devices.identification.node_uuid still "
                    "exists. Reapply the current device_id-only schema."
                )

            fk_rows = await connection.fetch(
                """
                SELECT
                    src_ns.nspname AS source_schema,
                    src.relname AS source_table,
                    dst_ns.nspname AS target_schema,
                    dst.relname AS target_table
                FROM pg_constraint c
                JOIN pg_class src ON src.oid = c.conrelid
                JOIN pg_namespace src_ns ON src_ns.oid = src.relnamespace
                JOIN pg_class dst ON dst.oid = c.confrelid
                JOIN pg_namespace dst_ns ON dst_ns.oid = dst.relnamespace
                WHERE c.contype = 'f'
                  AND (src_ns.nspname, src.relname) IN (
                      ('monitoring', 'readings'),
                      ('fall_detection', 'classification'),
                      ('fall_detection', 'activity_log')
                  )
                """
            )

            valid_sources: set[tuple[str, str]] = set()
            for row in fk_rows:
                source = (str(row["source_schema"]), str(row["source_table"]))
                if (
                    str(row["target_schema"]) == "monitoring"
                    and str(row["target_table"]) == "sessions"
                ):
                    valid_sources.add(source)

            missing_fks = expected_session_fks - valid_sources
            if missing_fks:
                formatted = ", ".join(
                    f"{schema}.{table}" for schema, table in sorted(missing_fks)
                )
                raise RuntimeError(
                    "Old/incorrect session foreign keys detected for: "
                    f"{formatted}. Their session_id foreign keys must reference "
                    "monitoring.sessions(session_id). Reapply the current schema."
                )

    async def authenticate(
        self,
        device_id: str,
        bearer_token: str,
    ) -> tuple[bool, str, bool]:
        """Validate a telemetry bearer token before any database write.

        Returns (accepted, reason, state_changed). State changes are used by the
        MQTT ingress to send a single accepted/rejected response to the node.
        """
        pool = self._require_pool()
        supplied_hash = (
            hashlib.sha256(bearer_token.encode("utf-8")).hexdigest()
            if bearer_token
            else None
        )

        async with pool.acquire() as connection:
            row = await connection.fetchrow(
                """
                SELECT token_hash
                FROM public.node_registrations
                WHERE device_id = $1
                  AND active = TRUE
                  AND approval_status = 'approved'
                  AND token_hash IS NOT NULL
                  AND token_revoked_at IS NULL
                """,
                device_id,
            )

            if row is None:
                accepted = False
                reason = "registration_not_approved"
            elif supplied_hash is None:
                accepted = False
                reason = "missing_bearer_token"
            else:
                expected_hash = str(row["token_hash"])
                accepted = hmac.compare_digest(supplied_hash, expected_hash)
                reason = "accepted" if accepted else "invalid_bearer_token"

            now = time.monotonic()
            last_update = self.last_auth_db_update.get(device_id, 0.0)
            if now - last_update >= LAST_SEEN_UPDATE_SECONDS:
                self.last_auth_db_update[device_id] = now
                if accepted:
                    await connection.execute(
                        """
                        UPDATE public.node_registrations
                        SET last_authenticated_at = now(),
                            last_seen_at = now()
                        WHERE device_id = $1
                          AND active = TRUE
                          AND approval_status = 'approved'
                        """,
                        device_id,
                    )
                else:
                    await connection.execute(
                        """
                        UPDATE public.node_registrations
                        SET last_auth_failure_at = now()
                        WHERE device_id = $1
                          AND active = TRUE
                        """,
                        device_id,
                    )

        previous = self.auth_state.get(device_id)
        previous_reason = self.auth_failure_reason.get(device_id)
        changed = previous is None or previous != accepted
        if not accepted and previous_reason != reason:
            changed = True

        self.auth_state[device_id] = accepted
        if accepted:
            self.auth_failure_reason.pop(device_id, None)
        else:
            self.auth_failure_reason[device_id] = reason

        if not accepted and changed:
            print(f"[WARN] MQTT auth rejected for {device_id}: {reason}")
        elif accepted and changed:
            print(f"[AUTH] MQTT bearer token accepted for {device_id}")

        return accepted, reason, changed

    async def get_or_create_session(
        self,
        device_id: str,
    ) -> int | None:
        pool = self._require_pool()

        async with pool.acquire() as connection:
            registration = await connection.fetchrow(
                """
                SELECT registration_id
                FROM public.node_registrations
                WHERE device_id = $1
                  AND active = TRUE
                  AND approval_status = 'approved'
                  AND token_hash IS NOT NULL
                  AND token_revoked_at IS NULL
                """,
                device_id,
            )

            if registration is None:
                if device_id not in self.warned_unregistered_devices:
                    print(
                        f"[WARN] Device {device_id} has no approved active registration"
                    )
                    self.warned_unregistered_devices.add(device_id)
                return None

            # Registration is active again, so allow a future loss of
            # registration to emit one new warning.
            self.warned_unregistered_devices.discard(device_id)

            identification = await connection.fetchrow(
                """
                SELECT device_id
                FROM devices.identification
                WHERE device_id = $1
                """,
                device_id,
            )
            if identification is None:
                raise RuntimeError(
                    f"Device {device_id} is missing from devices.identification"
                )

            row = await connection.fetchrow(
                """
                SELECT session_id
                FROM monitoring.sessions
                WHERE device_id = $1
                  AND end_time IS NULL
                ORDER BY start_time DESC
                LIMIT 1
                """,
                device_id,
            )
            if row is not None:
                return int(row["session_id"])

            row = await connection.fetchrow(
                """
                INSERT INTO monitoring.sessions (
                    device_id,
                    sample_rate_hz
                )
                VALUES ($1, $2)
                RETURNING session_id
                """,
                device_id,
                SAMPLE_RATE,
            )
            return int(row["session_id"])

    async def _ensure_session(self, device_id: str, state: DeviceState) -> None:
        """Recover cleanly if the database schema/session was recreated."""
        pool = self._require_pool()
        async with pool.acquire() as connection:
            exists = await connection.fetchval(
                """
                SELECT EXISTS (
                    SELECT 1
                    FROM monitoring.sessions
                    WHERE session_id = $1
                      AND device_id = $2
                      AND end_time IS NULL
                )
                """,
                state.session_id,
                device_id,
            )

        if exists:
            return

        # A DROP/CREATE migration invalidates in-memory session ids. Discard
        # any buffered readings tied to the old id and create/find a new one.
        state.reading_buffer.clear()
        session_id = await self.get_or_create_session(device_id)
        if session_id is None:
            self.device_states.pop(device_id, None)
            return

        state.session_id = session_id
        state.detector = FallDetector(sample_rate=SAMPLE_RATE)
        state.last_logged_activity = None
        state.last_sequence = None
        print(
            f"[DB] Re-created monitoring session for {device_id}: "
            f"session_id={state.session_id}"
        )

    async def _flush_readings(self, state: DeviceState) -> None:
        if not state.reading_buffer:
            return

        rows = state.reading_buffer
        state.reading_buffer = []
        pool = self._require_pool()

        async with pool.acquire() as connection:
            await connection.executemany(
                """
                INSERT INTO monitoring.readings (
                    session_id,
                    ts,
                    ax,
                    ay,
                    az,
                    gx,
                    gy,
                    gz,
                    smv,
                    gyro_magnitude
                )
                VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)
                """,
                rows,
            )

    async def _update_last_seen(
        self,
        state: DeviceState,
        device_id: str,
    ) -> None:
        now = time.monotonic()
        if now - state.last_seen_update < LAST_SEEN_UPDATE_SECONDS:
            return

        state.last_seen_update = now
        pool = self._require_pool()

        async with pool.acquire() as connection:
            async with connection.transaction():
                await connection.execute(
                    """
                    UPDATE devices.identification
                    SET last_seen_at = now()
                    WHERE device_id = $1
                    """,
                    device_id,
                )
                await connection.execute(
                    """
                    UPDATE public.node_registrations
                    SET last_seen_at = now()
                    WHERE device_id = $1
                      AND active = TRUE
                      AND approval_status = 'approved'
                    """,
                    device_id,
                )

    async def handle_reading(self, message: dict[str, Any]) -> None:
        if int(message.get("schema_version", 0)) != 1:
            raise ValueError("Unsupported or missing schema_version")

        device_id = normalize_device_id(str(message["device_id"]))
        sequence = int(message["sequence"])
        source_uptime_ms = int(message["source_uptime_ms"])

        ts = datetime.fromtimestamp(
            int(message["ts"]) / 1000.0,
            tz=timezone.utc,
        )

        ax = float(message["ax"])
        ay = float(message["ay"])
        az = float(message["az"])
        gx = float(message.get("gx", 0.0))
        gy = float(message.get("gy", 0.0))
        gz = float(message.get("gz", 0.0))

        state = self.device_states.get(device_id)
        if state is None:
            session_id = await self.get_or_create_session(device_id)
            if session_id is None:
                return

            state = DeviceState(session_id)
            self.device_states[device_id] = state
        else:
            await self._ensure_session(device_id, state)
            state = self.device_states.get(device_id)
            if state is None:
                return

        if state.last_sequence == sequence:
            return
        state.last_sequence = sequence

        smv = (ax**2 + ay**2 + az**2) ** 0.5
        gyro_magnitude = (gx**2 + gy**2 + gz**2) ** 0.5

        state.reading_buffer.append(
            (
                state.session_id,
                ts,
                ax,
                ay,
                az,
                gx,
                gy,
                gz,
                smv,
                gyro_magnitude,
            )
        )
        if len(state.reading_buffer) >= READING_BATCH_SIZE:
            await self._flush_readings(state)

        await self._update_last_seen(state, device_id)

        # Detector timing follows the ESP32 monotonic source clock instead of
        # MQTT arrival time, so Wi-Fi/broker jitter does not alter fall windows.
        detector_time = source_uptime_ms / 1000.0
        result = state.detector.add_sample(
            ax,
            ay,
            az,
            detector_time,
            gx,
            gy,
            gz,
        )

        pool = self._require_pool()
        if result:
            async with pool.acquire() as connection:
                await connection.execute(
                    """
                    INSERT INTO fall_detection.classification (
                        session_id,
                        ts,
                        event_type,
                        fall_type,
                        max_smv,
                        max_rotation,
                        freefall_duration,
                        tilt_change,
                        responsiveness,
                        activity_before_event
                    )
                    VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)
                    """,
                    state.session_id,
                    ts,
                    result["event_type"],
                    result["fall_type"],
                    result["max_smv"],
                    result["max_rotation"],
                    result["freefall_duration"],
                    result["tilt_change"],
                    result["responsiveness"],
                    result["activity_before_event"],
                )

        current_activity = state.detector.current_activity
        if (
            current_activity
            and current_activity != "unknown"
            and current_activity != state.last_logged_activity
        ):
            state.last_logged_activity = current_activity
            async with pool.acquire() as connection:
                await connection.execute(
                    """
                    INSERT INTO fall_detection.activity_log (
                        session_id,
                        ts,
                        activity_state
                    )
                    VALUES ($1,$2,$3)
                    """,
                    state.session_id,
                    ts,
                    current_activity,
                )


class MqttIngress:
    def __init__(self, loop: asyncio.AbstractEventLoop) -> None:
        self.loop = loop
        self.queue: asyncio.Queue[tuple[str, dict[str, Any] | list[Any]]] = asyncio.Queue(
            maxsize=MQTT_QUEUE_SIZE
        )
        self.dropped_messages = 0

        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id="tervanix-postgres-writer",
        )
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)

        if MQTT_USERNAME:
            self.client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    def _on_connect(
        self,
        client: mqtt.Client,
        userdata: Any,
        flags: Any,
        reason_code: Any,
        properties: Any,
    ) -> None:
        del userdata, flags, properties
        if bool(getattr(reason_code, "is_failure", False)):
            print(f"[MQTT] connection failed: {reason_code}")
            return

        result, _mid = client.subscribe(MQTT_TOPIC, qos=MQTT_QOS)
        if result != mqtt.MQTT_ERR_SUCCESS:
            print(f"[MQTT] subscribe failed rc={result}")
            return

        print(
            f"[MQTT] connected to {MQTT_HOST}:{MQTT_PORT}; "
            f"subscribed to {MQTT_TOPIC}"
        )

    def _on_disconnect(
        self,
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: Any,
        reason_code: Any,
        properties: Any,
    ) -> None:
        del client, userdata, disconnect_flags, properties
        print(f"[MQTT] disconnected: {reason_code}")

    def _enqueue(
        self,
        item: tuple[str, dict[str, Any] | list[Any]],
    ) -> None:
        try:
            self.queue.put_nowait(item)
        except asyncio.QueueFull:
            self.dropped_messages += 1
            if self.dropped_messages % 500 == 1:
                print(
                    "[WARN] MQTT processing queue is full; "
                    f"dropped={self.dropped_messages}"
                )

    def _on_message(
        self,
        client: mqtt.Client,
        userdata: Any,
        message: mqtt.MQTTMessage,
    ) -> None:
        del client, userdata
        try:
            payload = json.loads(message.payload.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as error:
            print(f"[WARN] Invalid MQTT JSON on {message.topic}: {error}")
            return

        if not isinstance(payload, (dict, list)):
            print(f"[WARN] Ignoring non-object MQTT payload on {message.topic}")
            return

        self.loop.call_soon_threadsafe(
            self._enqueue,
            (message.topic, payload),
        )

    def publish_auth_status(
        self,
        device_id: str,
        accepted: bool,
        reason: str,
    ) -> None:
        compact = device_id.replace(":", "")
        topic = f"tervanix/devices/{compact}/auth/status"
        payload = json.dumps(
            {
                "status": "accepted" if accepted else "rejected",
                "reason": reason,
            },
            separators=(",", ":"),
        )
        info = self.client.publish(topic, payload, qos=1, retain=False)
        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            print(f"[WARN] Failed to publish MQTT auth status for {device_id}: rc={info.rc}")

    def start(self) -> None:
        self.client.connect_async(MQTT_HOST, MQTT_PORT, MQTT_KEEPALIVE)
        self.client.loop_start()

    def stop(self) -> None:
        try:
            self.client.disconnect()
        finally:
            self.client.loop_stop()


async def main() -> None:
    pipeline = Pipeline()
    await pipeline.connect()

    db_name = "DSN" if "dsn" in DB_CONFIG else DB_CONFIG.get("database")
    print(f"Connected to Postgres ({db_name})")

    ingress = MqttIngress(asyncio.get_running_loop())
    ingress.start()

    try:
        while True:
            topic, payload = await ingress.queue.get()

            topic_parts = topic.split("/")
            topic_device = (
                topic_parts[2].upper()
                if len(topic_parts) == 4
                and topic_parts[0] == "tervanix"
                and topic_parts[1] == "devices"
                and topic_parts[3] == "telemetry"
                else None
            )

            if not isinstance(payload, dict):
                print(f"[WARN] Ignoring unauthenticated legacy MQTT payload on {topic}")
                continue

            try:
                if int(payload.get("schema_version", 0)) != 1:
                    raise ValueError("Unsupported or missing envelope schema_version")

                device_id = normalize_device_id(str(payload["device_id"]))
                if topic_device is None or device_id.replace(":", "") != topic_device:
                    raise ValueError(
                        f"MQTT topic/device_id mismatch: topic={topic} payload={device_id}"
                    )

                bearer_token = str(payload.get("bearer_token") or "")
                accepted, reason, changed = await pipeline.authenticate(
                    device_id,
                    bearer_token,
                )

                if changed:
                    ingress.publish_auth_status(device_id, accepted, reason)
                if not accepted:
                    continue

                readings = payload.get("readings")
                if not isinstance(readings, list):
                    raise ValueError("Telemetry envelope readings must be a JSON array")

                for reading in readings:
                    if not isinstance(reading, dict):
                        print("[WARN] Ignoring non-object reading in MQTT batch")
                        continue

                    normalized = dict(reading)
                    normalized["schema_version"] = 1
                    normalized["device_id"] = device_id
                    try:
                        await pipeline.handle_reading(normalized)
                    except Exception as error:
                        print(f"[WARN] Failed to process MQTT reading: {error}")
            except Exception as error:
                print(f"[WARN] Failed to process MQTT envelope: {error}")
    finally:
        ingress.stop()
        await pipeline.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
