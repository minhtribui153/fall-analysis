"""Decrypt Nesso N1 MQTT telemetry and persist worker-centric monitoring data.

Registration/assignment happens over BLE. An administrator assigns a disposable
Nesso N1 device to a durable worker. Approval creates an expiring
monitoring.sessions row and provisions the node with a bearer token.

MQTT telemetry uses application-layer AES-256-GCM:
    topic: tervanix/sessions/<32-hex-session-uuid>/telemetry
    payload: {
      "schema_version": 2,
      "alg": "A256GCM",
      "session_id": "...",
      "worker_id": "...",
      "nonce": "base64",
      "ciphertext": "base64",
      "tag": "base64"
    }

The bearer token is never present in MQTT JSON. This process deterministically
re-derives the token from REGISTRATION_TOKEN_SECRET + session database metadata,
checks its stored SHA-256 hash, derives AES-256 key = SHA256(token), and decrypts
only while the session is active and unexpired.
"""

from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import json
import os
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

import asyncpg
import paho.mqtt.client as mqtt
from cryptography.exceptions import InvalidTag
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from dotenv import load_dotenv

from detection import FallDetector

load_dotenv()

MQTT_HOST = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_KEEPALIVE = int(os.getenv("MQTT_KEEPALIVE", "60"))
MQTT_TOPIC = os.getenv("MQTT_TELEMETRY_SUBSCRIPTION", "tervanix/sessions/+/telemetry")
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
        raise RuntimeError("Set DATABASE_URL or define: " + ", ".join(missing))
    return {
        "host": os.getenv("POSTGRES_HOST", "localhost"),
        "port": int(os.getenv("POSTGRES_PORT", "5432")),
        "database": os.environ["POSTGRES_DB"],
        "user": os.environ["POSTGRES_USER"],
        "password": os.environ["POSTGRES_PASSWORD"],
    }


DB_CONFIG = make_db_config()


def load_token_secret() -> bytes:
    configured = os.getenv("REGISTRATION_TOKEN_SECRET", "").strip()
    if configured:
        if len(configured) < 32:
            raise RuntimeError("REGISTRATION_TOKEN_SECRET must be at least 32 characters")
        return configured.encode()
    default_file = Path(__file__).resolve().with_name(".registration_token_secret")
    path = Path(os.getenv("REGISTRATION_TOKEN_SECRET_FILE", str(default_file)))
    if not path.exists():
        raise RuntimeError(
            f"Registration token secret not found at {path}; run this beside n1_registration_server.py "
            "or set REGISTRATION_TOKEN_SECRET_FILE"
        )
    value = path.read_text(encoding="utf-8").strip()
    if len(value) < 32:
        raise RuntimeError(f"Token secret in {path} is too short")
    return value.encode()


TOKEN_SECRET = load_token_secret()


def derive_bearer_token(
    session_id: uuid.UUID,
    device_id: str,
    worker_id: str,
    worker_uuid: uuid.UUID,
    token_issued_at: datetime,
    token_expires_at: datetime,
) -> str:
    material = (
        f"v2|{session_id}|{device_id}|{worker_id}|{worker_uuid}|"
        f"{int(token_issued_at.timestamp())}|{int(token_expires_at.timestamp())}"
    ).encode()
    digest = hmac.new(TOKEN_SECRET, material, hashlib.sha256).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")


def compact_session_id(value: uuid.UUID) -> str:
    return value.hex.upper()


def session_aad(session_id: uuid.UUID, worker_id: str, message_type: str) -> bytes:
    return f"tervanix|v2|{session_id}|{worker_id}|{message_type}".encode("utf-8")


class SessionState:
    def __init__(self, session_id: uuid.UUID, device_id: str, worker_id: str) -> None:
        self.session_id = session_id
        self.device_id = device_id
        self.worker_id = worker_id
        self.detector = FallDetector(sample_rate=SAMPLE_RATE)
        self.reading_buffer: list[tuple[Any, ...]] = []
        self.last_logged_activity: str | None = None
        self.last_seen_update = 0.0
        self.last_sequence: int | None = None


class Pipeline:
    def __init__(self) -> None:
        self.pool: asyncpg.Pool | None = None
        self.session_states: dict[uuid.UUID, SessionState] = {}
        self.auth_state: dict[uuid.UUID, tuple[bool, str]] = {}

    async def connect(self) -> None:
        self.pool = await asyncpg.create_pool(**DB_CONFIG)
        await self._validate_schema()

    async def close(self) -> None:
        if self.pool is None:
            return
        for state in self.session_states.values():
            await self._flush_readings(state)
        await self.pool.close()
        self.pool = None

    def _require_pool(self) -> asyncpg.Pool:
        if self.pool is None:
            raise RuntimeError("Postgres pool is not connected")
        return self.pool

    async def _validate_schema(self) -> None:
        required: dict[tuple[str, str], set[str]] = {
            ("identification", "workers"): {"worker_id", "worker_uuid", "worker_name"},
            ("identification", "devices"): {"device_id", "ble_address"},
            ("monitoring", "sessions"): {
                "session_id", "device_id", "worker_id", "bearer_token_hash",
                "token_issued_at", "token_expires_at", "token_revoked_at",
                "active", "last_authenticated_at", "last_auth_failure_at",
            },
            ("monitoring", "readings"): {"session_id", "device_id", "sequence", "ts", "ax", "ay", "az"},
            ("fall_detection", "activity_log"): {"session_id", "worker_id", "prediction_sequence", "ts", "activity_state", "confidence", "model_sha256", "source"},
            ("fall_detection", "classification"): {"session_id", "worker_id", "prediction_sequence", "ts", "event_type", "predicted_state", "confidence", "model_sha256", "source"},
        }
        pool = self._require_pool()
        async with pool.acquire() as connection:
            for (schema, table), expected in required.items():
                rows = await connection.fetch(
                    """
                    SELECT column_name FROM information_schema.columns
                    WHERE table_schema=$1 AND table_name=$2
                    """,
                    schema,
                    table,
                )
                actual = {str(row["column_name"]) for row in rows}
                missing = expected - actual
                if missing:
                    raise RuntimeError(
                        f"Database schema mismatch for {schema}.{table}: missing {sorted(missing)}"
                    )

    async def authenticate_and_decrypt(
        self,
        envelope: dict[str, Any],
        topic_session_hex: str,
    ) -> tuple[dict[str, Any] | None, str, bool]:
        try:
            if int(envelope.get("schema_version", 0)) != 2:
                raise ValueError("unsupported_schema_version")
            if envelope.get("alg") != "A256GCM":
                raise ValueError("unsupported_encryption_algorithm")
            session_id = uuid.UUID(str(envelope["session_id"]))
            worker_id = str(envelope["worker_id"]).strip().upper()
            message_type = str(envelope.get("message_type") or "telemetry")
            if message_type != "telemetry":
                raise ValueError("unexpected_message_type")
        except (KeyError, ValueError) as error:
            return None, str(error), True

        if compact_session_id(session_id) != topic_session_hex.upper():
            return None, "topic_session_mismatch", True

        pool = self._require_pool()
        async with pool.acquire() as connection:
            row = await connection.fetchrow(
                """
                SELECT
                    s.device_id, s.worker_id, w.worker_uuid,
                    s.bearer_token_hash, s.token_issued_at, s.token_expires_at,
                    s.token_revoked_at, s.active
                FROM monitoring.sessions s
                JOIN identification.workers w ON w.worker_id=s.worker_id
                WHERE s.session_id=$1
                """,
                session_id,
            )
            if row is None:
                return None, "unknown_session", self._auth_changed(session_id, False, "unknown_session")

            now = datetime.now(timezone.utc)
            if (
                not bool(row["active"])
                or row["token_revoked_at"] is not None
                or row["token_expires_at"] <= now
            ):
                await connection.execute(
                    "UPDATE monitoring.sessions SET last_auth_failure_at=now() WHERE session_id=$1",
                    session_id,
                )
                return None, "session_expired_or_revoked", self._auth_changed(
                    session_id, False, "session_expired_or_revoked"
                )

            stored_worker_id = str(row["worker_id"])
            if worker_id != stored_worker_id:
                return None, "worker_session_mismatch", self._auth_changed(
                    session_id, False, "worker_session_mismatch"
                )

            token = derive_bearer_token(
                session_id,
                str(row["device_id"]),
                stored_worker_id,
                uuid.UUID(str(row["worker_uuid"])),
                row["token_issued_at"],
                row["token_expires_at"],
            )
            token_hash = hashlib.sha256(token.encode()).hexdigest()
            if not hmac.compare_digest(token_hash, str(row["bearer_token_hash"])):
                return None, "token_derivation_mismatch", self._auth_changed(
                    session_id, False, "token_derivation_mismatch"
                )

            try:
                nonce = base64.b64decode(str(envelope["nonce"]), validate=True)
                ciphertext = base64.b64decode(str(envelope["ciphertext"]), validate=True)
                tag = base64.b64decode(str(envelope["tag"]), validate=True)
                if len(nonce) != 12 or len(tag) != 16:
                    raise ValueError("invalid_gcm_lengths")
                key = hashlib.sha256(token.encode()).digest()
                plaintext = AESGCM(key).decrypt(
                    nonce,
                    ciphertext + tag,
                    session_aad(session_id, worker_id),
                )
                payload = json.loads(plaintext.decode("utf-8"))
            except (KeyError, ValueError, UnicodeDecodeError, json.JSONDecodeError, InvalidTag):
                await connection.execute(
                    "UPDATE monitoring.sessions SET last_auth_failure_at=now() WHERE session_id=$1",
                    session_id,
                )
                return None, "decrypt_or_auth_failed", self._auth_changed(
                    session_id, False, "decrypt_or_auth_failed"
                )

            if not isinstance(payload, dict):
                return None, "decrypted_payload_not_object", True
            if str(payload.get("session_id")) != str(session_id):
                return None, "encrypted_session_mismatch", True
            if str(payload.get("worker_id", "")).upper() != worker_id:
                return None, "encrypted_worker_mismatch", True
            if str(payload.get("device_id", "")) != str(row["device_id"]):
                return None, "encrypted_device_mismatch", True

            await connection.execute(
                """
                UPDATE monitoring.sessions
                SET last_authenticated_at=now()
                WHERE session_id=$1
                """,
                session_id,
            )

        payload["_session_id"] = session_id
        payload["_worker_id"] = worker_id
        payload["_device_id"] = str(row["device_id"])
        return payload, "accepted", self._auth_changed(session_id, True, "accepted")

    def _auth_changed(self, session_id: uuid.UUID, accepted: bool, reason: str) -> bool:
        previous = self.auth_state.get(session_id)
        current = (accepted, reason)
        self.auth_state[session_id] = current
        changed = previous != current
        if changed:
            if accepted:
                print(f"[AUTH] Encrypted MQTT session accepted: {session_id}")
            else:
                print(f"[WARN] Encrypted MQTT session rejected {session_id}: {reason}")
        return changed

    async def _flush_readings(self, state: SessionState) -> None:
        if not state.reading_buffer:
            return
        rows = state.reading_buffer
        state.reading_buffer = []
        pool = self._require_pool()
        async with pool.acquire() as connection:
            await connection.executemany(
                """
                INSERT INTO monitoring.readings(
                    session_id, device_id, sequence, ts, ax, ay, az, gx, gy, gz, smv, gyro_magnitude
                ) VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12)
                ON CONFLICT (session_id, sequence) DO NOTHING
                """,
                rows,
            )

    async def _handle_device_prediction(self, payload: dict[str, Any]) -> None:
        prediction = payload.get("prediction")
        if prediction is None:
            return
        if not isinstance(prediction, dict):
            raise ValueError("prediction must be an object")
        session_id = payload["_session_id"]
        worker_id = payload["_worker_id"]
        prediction_sequence = int(prediction["prediction_sequence"])
        ts = datetime.fromtimestamp(int(prediction["ts"]) / 1000.0, tz=timezone.utc)
        state = str(prediction["state"]).strip().lower()
        confidence = float(prediction["confidence"])
        model_sha256 = str(prediction["model_sha256"]).strip().lower()
        source = str(prediction.get("source") or "nesso_tflite").strip().lower()
        allowed = {"falling_from_height","jumping","near_miss","running","slip","stationary","trip","unknown","walking"}
        if state not in allowed:
            raise ValueError(f"unsupported prediction state: {state}")
        if len(model_sha256) != 64 or any(ch not in "0123456789abcdef" for ch in model_sha256):
            raise ValueError("invalid prediction model_sha256")
        if source != "nesso_tflite":
            raise ValueError("unsupported prediction source")
        pool = self._require_pool()
        async with pool.acquire() as connection:
            await connection.execute("""
                INSERT INTO fall_detection.activity_log(
                    session_id, worker_id, prediction_sequence, ts,
                    activity_state, confidence, model_sha256, source)
                VALUES ($1,$2,$3,$4,$5,$6,$7,$8)
                ON CONFLICT (session_id, prediction_sequence)
                    WHERE prediction_sequence IS NOT NULL DO NOTHING
            """, session_id, worker_id, prediction_sequence, ts, state, confidence, model_sha256, source)
            event_type = None
            fall_type = None
            if state == "falling_from_height":
                event_type, fall_type = "FALL", "FFH"
            elif state in {"near_miss", "trip", "slip"}:
                event_type = "NEAR_MISS"
            if event_type is not None:
                await connection.execute("""
                    INSERT INTO fall_detection.classification(
                        session_id, worker_id, prediction_sequence, ts,
                        event_type, fall_type, predicted_state, confidence,
                        model_sha256, source)
                    VALUES ($1,$2,$3,$4,$5,$6,$7,$8,$9,$10)
                    ON CONFLICT (session_id, prediction_sequence, source)
                        WHERE prediction_sequence IS NOT NULL DO NOTHING
                """, session_id, worker_id, prediction_sequence, ts,
                    event_type, fall_type, state, confidence, model_sha256, source)

    async def handle_payload(self, payload: dict[str, Any]) -> None:
        session_id = payload["_session_id"]
        worker_id = payload["_worker_id"]
        device_id = payload["_device_id"]
        await self._handle_device_prediction(payload)
        readings = payload.get("readings")
        if not isinstance(readings, list):
            raise ValueError("decrypted payload readings must be an array")

        state = self.session_states.get(session_id)
        if state is None:
            state = SessionState(session_id, device_id, worker_id)
            pool = self._require_pool()
            async with pool.acquire() as connection:
                last_sequence = await connection.fetchval(
                    "SELECT max(sequence) FROM monitoring.readings WHERE session_id=$1",
                    session_id,
                )
            state.last_sequence = int(last_sequence) if last_sequence is not None else None
            self.session_states[session_id] = state

        pool = self._require_pool()
        for reading in readings:
            if not isinstance(reading, dict):
                continue
            sequence = int(reading["sequence"])
            if state.last_sequence is not None and sequence <= state.last_sequence:
                continue
            state.last_sequence = sequence

            source_uptime_ms = int(reading["source_uptime_ms"])
            ts = datetime.fromtimestamp(int(reading["ts"]) / 1000.0, tz=timezone.utc)
            ax = float(reading["ax"])
            ay = float(reading["ay"])
            az = float(reading["az"])
            gx = float(reading.get("gx", 0.0))
            gy = float(reading.get("gy", 0.0))
            gz = float(reading.get("gz", 0.0))
            smv = (ax**2 + ay**2 + az**2) ** 0.5
            gyro_magnitude = (gx**2 + gy**2 + gz**2) ** 0.5

            state.reading_buffer.append(
                (session_id, device_id, sequence, ts, ax, ay, az, gx, gy, gz, smv, gyro_magnitude)
            )
            if len(state.reading_buffer) >= READING_BATCH_SIZE:
                await self._flush_readings(state)

            detector_time = source_uptime_ms / 1000.0
            result = state.detector.add_sample(ax, ay, az, detector_time, gx, gy, gz)
            if result:
                async with pool.acquire() as connection:
                    await connection.execute(
                        """
                        INSERT INTO fall_detection.classification(
                            session_id, worker_id, prediction_sequence, ts, event_type, fall_type,
                            max_smv, max_rotation, freefall_duration, tilt_change,
                            responsiveness, activity_before_event, source
                        ) VALUES ($1,$2,NULL,$3,$4,$5,$6,$7,$8,$9,$10,$11,'server_rules')
                        """,
                        session_id,
                        worker_id,
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
                        INSERT INTO fall_detection.activity_log(
                            session_id, worker_id, prediction_sequence, ts, activity_state, source
                        ) VALUES ($1,$2,NULL,$3,$4,'server_rules')
                        """,
                        session_id,
                        worker_id,
                        ts,
                        current_activity,
                    )

        now = time.monotonic()
        if now - state.last_seen_update >= LAST_SEEN_UPDATE_SECONDS:
            state.last_seen_update = now
            async with pool.acquire() as connection:
                await connection.execute(
                    "UPDATE identification.devices SET last_seen_at=now() WHERE device_id=$1",
                    device_id,
                )


class MqttIngress:
    def __init__(self, loop: asyncio.AbstractEventLoop) -> None:
        self.loop = loop
        self.queue: asyncio.Queue[tuple[str, dict[str, Any]]] = asyncio.Queue(maxsize=MQTT_QUEUE_SIZE)
        self.client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id="tervanix-postgres-writer")
        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.on_message = self._on_message
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        if MQTT_USERNAME:
            self.client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)

    def _on_connect(self, client: mqtt.Client, userdata: Any, flags: Any, reason_code: Any, properties: Any) -> None:
        del userdata, flags, properties
        if bool(getattr(reason_code, "is_failure", False)):
            print(f"[MQTT] connection failed: {reason_code}")
            return
        client.subscribe(MQTT_TOPIC, qos=MQTT_QOS)
        print(f"[MQTT] connected to {MQTT_HOST}:{MQTT_PORT}; subscribed to {MQTT_TOPIC}")

    def _on_disconnect(self, client: mqtt.Client, userdata: Any, disconnect_flags: Any, reason_code: Any, properties: Any) -> None:
        del client, userdata, disconnect_flags, properties
        print(f"[MQTT] disconnected: {reason_code}")

    def _on_message(self, client: mqtt.Client, userdata: Any, message: mqtt.MQTTMessage) -> None:
        del client, userdata
        try:
            payload = json.loads(message.payload.decode("utf-8"))
            if not isinstance(payload, dict):
                raise ValueError("envelope must be an object")
        except (UnicodeDecodeError, json.JSONDecodeError, ValueError) as error:
            print(f"[WARN] Invalid encrypted MQTT envelope on {message.topic}: {error}")
            return
        self.loop.call_soon_threadsafe(self._enqueue, (message.topic, payload))

    def _enqueue(self, item: tuple[str, dict[str, Any]]) -> None:
        try:
            self.queue.put_nowait(item)
        except asyncio.QueueFull:
            print("[WARN] MQTT processing queue full; dropping encrypted telemetry")

    def publish_auth_status(self, session_id: uuid.UUID, accepted: bool, reason: str) -> None:
        topic = f"tervanix/sessions/{compact_session_id(session_id)}/auth/status"
        payload = json.dumps(
            {"status": "accepted" if accepted else "rejected", "reason": reason},
            separators=(",", ":"),
        )
        self.client.publish(topic, payload, qos=1, retain=False)

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
    print("Connected to Postgres; encrypted worker/session pipeline ready")
    ingress = MqttIngress(asyncio.get_running_loop())
    ingress.start()
    try:
        while True:
            topic, envelope = await ingress.queue.get()
            parts = topic.split("/")
            if not (
                len(parts) == 4
                and parts[0] == "tervanix"
                and parts[1] == "sessions"
                and parts[3] == "telemetry"
                and len(parts[2]) == 32
            ):
                continue
            payload, reason, changed = await pipeline.authenticate_and_decrypt(envelope, parts[2])
            try:
                session_id = uuid.UUID(str(envelope.get("session_id")))
            except ValueError:
                session_id = None
            if changed and session_id is not None:
                ingress.publish_auth_status(session_id, payload is not None, reason)
            if payload is None:
                continue
            try:
                await pipeline.handle_payload(payload)
            except Exception as error:
                print(f"[WARN] Failed to process decrypted MQTT payload: {error}")
    finally:
        ingress.stop()
        await pipeline.close()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
