from __future__ import annotations

import asyncio
import base64
import hashlib
import hmac
import ipaddress
import json
import logging
import os
import secrets
import signal
import struct
import uuid
from dataclasses import dataclass
from datetime import datetime, timedelta, timezone
from pathlib import Path
from typing import Any

import psycopg
from aiohttp import web
from bless import (
    BlessGATTCharacteristic,
    BlessServer,
    GATTAttributePermissions,
    GATTCharacteristicProperties,
)
from dbus_next.errors import DBusError
from dotenv import load_dotenv

load_dotenv()

logging.basicConfig(
    level=os.getenv("LOG_LEVEL", "INFO").upper(),
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
logger = logging.getLogger("tervanix.registration")

SERVER_NAME = "TVX-REG"
SERVICE_UUID = "7db20000-6e8d-4d8d-a991-2cf812340000"
REGISTRATION_UUID = "7db20001-6e8d-4d8d-a991-2cf812340000"
CREDENTIAL_UUID = "7db20002-6e8d-4d8d-a991-2cf812340000"
WORKER_CONTEXT_UUID = "7db20003-6e8d-4d8d-a991-2cf812340000"
WORKER_NAME_UUID = "7db20004-6e8d-4d8d-a991-2cf812340000"

PROTOCOL_VERSION = 2
FLAG_HAS_UUID = 1 << 0
FLAG_HAS_SERVER_UUID = 1 << 1

STATUS_ERROR = 0
STATUS_EXISTING_ACCEPTED = 1
STATUS_NEW_ASSIGNED = 2
STATUS_IDENTITY_REJECTED = 3
STATUS_SERVER_REJECTED = 4

# Immutable ESP32 registration structs: 44-byte request, 40-byte response.
REQUEST_FORMAT = "<BB6s16s16sI"
RESPONSE_FORMAT = "<B16s16s3xI"
REQUEST_SIZE = struct.calcsize(REQUEST_FORMAT)
RESPONSE_SIZE = struct.calcsize(RESPONSE_FORMAT)
ZERO_UUID = uuid.UUID(int=0)

# Worker context is deliberately below the current 64-byte ATT MTU:
# version(1) + worker_id(12) + session UUID(16) + expiry epoch seconds(4) = 33.
WORKER_CONTEXT_VERSION = 1
WORKER_CONTEXT_FORMAT = "<B12s16sI"
WORKER_CONTEXT_SIZE = struct.calcsize(WORKER_CONTEXT_FORMAT)

WORKER_ID_LENGTH = 10
WORKER_NAME_MAX = 16
WORKER_ID_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"

ADMIN_HOST = os.getenv("REGISTRATION_ADMIN_HOST", "0.0.0.0")
ADMIN_PORT = int(os.getenv("REGISTRATION_ADMIN_PORT", "8765"))
ADMIN_TOKEN = os.getenv("REGISTRATION_ADMIN_TOKEN", "").strip()
SESSION_TTL_SECONDS = int(os.getenv("REGISTRATION_SESSION_TTL_SECONDS", "28800"))
SAMPLE_RATE_HZ = int(os.getenv("SAMPLE_RATE_HZ", "100"))
HAR_MODEL_PATH = Path(os.getenv("HAR_MODEL_PATH", str(Path(__file__).resolve().parent / "models" / "har_model.tflite")))
HAR_MODEL_ID = os.getenv("HAR_MODEL_ID", "").strip()
HAR_MODEL_MIN_CONFIDENCE = float(os.getenv("HAR_MODEL_MIN_CONFIDENCE", "0.60"))
HAR_MODEL_CLASSES = ("falling_from_height","jumping","near_miss","running","slip","stationary","trip","unknown","walking")

VALID_APPROVAL_STATUSES = {"pending", "approved", "rejected"}


def make_database_url() -> str:
    explicit = os.getenv("DATABASE_URL")
    if explicit:
        return explicit
    user = os.getenv("POSTGRES_USER")
    password = os.getenv("POSTGRES_PASSWORD")
    database = os.getenv("POSTGRES_DB")
    host = os.getenv("POSTGRES_HOST", "localhost")
    port = os.getenv("POSTGRES_PORT", "5432")
    missing = [
        name
        for name, value in (
            ("POSTGRES_USER", user),
            ("POSTGRES_PASSWORD", password),
            ("POSTGRES_DB", database),
        )
        if not value
    ]
    if missing:
        raise RuntimeError("Set DATABASE_URL or define: " + ", ".join(missing))
    return f"postgresql://{user}:{password}@{host}:{port}/{database}"


DATABASE_URL = make_database_url()


@dataclass(frozen=True)
class RegistrationRequest:
    device_id_bytes: bytes
    device_id: str
    current_uuid: uuid.UUID | None
    expected_server_uuid: uuid.UUID | None
    nonce: int


@dataclass(frozen=True)
class RegistrationResult:
    status: int
    node_uuid: uuid.UUID
    server_uuid: uuid.UUID
    reason: str


def format_device_id(raw: bytes) -> str:
    if len(raw) != 6:
        raise ValueError("device_id must contain exactly 6 bytes")
    return ":".join(f"{byte:02X}" for byte in raw)


def normalize_device_id(value: str) -> str:
    parts = value.strip().upper().split(":")
    if len(parts) != 6 or any(len(part) != 2 for part in parts):
        raise ValueError("device_id must use AA:BB:CC:DD:EE:FF format")
    try:
        bytes(int(part, 16) for part in parts)
    except ValueError as error:
        raise ValueError("device_id contains non-hexadecimal bytes") from error
    return ":".join(parts)


def normalize_worker_name(value: str) -> str:
    name = " ".join(value.strip().split())
    if not name:
        raise ValueError("worker_name is required")
    if len(name) > WORKER_NAME_MAX:
        raise ValueError(f"worker_name must be at most {WORKER_NAME_MAX} characters")
    if len(name.encode("utf-8")) > 60:
        raise ValueError("worker_name UTF-8 encoding is too large for BLE provisioning")
    return name


def generate_worker_id(connection: psycopg.Connection[Any]) -> str:
    for _ in range(100):
        candidate = "".join(secrets.choice(WORKER_ID_ALPHABET) for _ in range(WORKER_ID_LENGTH))
        exists = connection.execute(
            "SELECT 1 FROM identification.workers WHERE worker_id = %s",
            (candidate,),
        ).fetchone()
        if exists is None:
            return candidate
    raise RuntimeError("Could not allocate a unique worker_id")


def parse_registration_request(value: bytes) -> RegistrationRequest:
    if len(value) != REQUEST_SIZE:
        raise ValueError(f"Expected {REQUEST_SIZE} registration bytes, received {len(value)}")
    version, flags, device_bytes, current_raw, server_raw, nonce = struct.unpack(
        REQUEST_FORMAT, value
    )
    if version != PROTOCOL_VERSION:
        raise ValueError(f"Unsupported registration protocol version {version}")

    current_uuid = None
    if flags & FLAG_HAS_UUID:
        parsed = uuid.UUID(bytes=current_raw)
        if parsed.int:
            current_uuid = parsed

    expected_server_uuid = None
    if flags & FLAG_HAS_SERVER_UUID:
        parsed = uuid.UUID(bytes=server_raw)
        if parsed.int:
            expected_server_uuid = parsed

    return RegistrationRequest(
        device_id_bytes=device_bytes,
        device_id=format_device_id(device_bytes),
        current_uuid=current_uuid,
        expected_server_uuid=expected_server_uuid,
        nonce=nonce,
    )


def build_registration_response(result: RegistrationResult, nonce: int) -> bytes:
    return struct.pack(
        RESPONSE_FORMAT,
        result.status,
        result.node_uuid.bytes,
        result.server_uuid.bytes,
        nonce,
    )


def load_server_uuid() -> uuid.UUID:
    configured = os.getenv("REGISTRATION_SERVER_UUID", "").strip()
    if configured:
        return uuid.UUID(configured)
    path = Path(
        os.getenv(
            "REGISTRATION_SERVER_UUID_FILE",
            str(Path(__file__).resolve().with_name(".registration_server_uuid")),
        )
    )
    if path.exists():
        return uuid.UUID(path.read_text(encoding="utf-8").strip())
    value = uuid.uuid4()
    path.write_text(str(value) + "\n", encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass
    return value


def load_token_secret() -> bytes:
    configured = os.getenv("REGISTRATION_TOKEN_SECRET", "").strip()
    if configured:
        if len(configured) < 32:
            raise RuntimeError("REGISTRATION_TOKEN_SECRET must be at least 32 characters")
        return configured.encode()
    path = Path(
        os.getenv(
            "REGISTRATION_TOKEN_SECRET_FILE",
            str(Path(__file__).resolve().with_name(".registration_token_secret")),
        )
    )
    if path.exists():
        value = path.read_text(encoding="utf-8").strip()
        if len(value) < 32:
            raise RuntimeError(f"Token secret in {path} is too short")
        return value.encode()
    value = secrets.token_urlsafe(48)
    path.write_text(value + "\n", encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass
    return value.encode()


def derive_bearer_token(
    token_secret: bytes,
    session_id: uuid.UUID,
    device_id: str,
    worker_id: str,
    worker_uuid: uuid.UUID,
    token_issued_at: datetime,
    token_expires_at: datetime,
) -> str:
    if token_issued_at.tzinfo is None:
        token_issued_at = token_issued_at.replace(tzinfo=timezone.utc)
    if token_expires_at.tzinfo is None:
        token_expires_at = token_expires_at.replace(tzinfo=timezone.utc)
    material = (
        f"v2|{session_id}|{device_id}|{worker_id}|{worker_uuid}|"
        f"{int(token_issued_at.timestamp())}|{int(token_expires_at.timestamp())}"
    ).encode()
    digest = hmac.new(token_secret, material, hashlib.sha256).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")


def hash_bearer_token(token: str) -> str:
    return hashlib.sha256(token.encode()).hexdigest()


def current_model_descriptor() -> dict[str, Any]:
    if not HAR_MODEL_PATH.exists():
        raise RuntimeError(f"HAR model file does not exist: {HAR_MODEL_PATH}")
    data = HAR_MODEL_PATH.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    return {"model_id": HAR_MODEL_ID or f"har-{digest[:12]}", "sha256": digest, "size": len(data), "sample_rate_hz": 25, "window_samples": 50, "slide_samples": 25, "num_axes": 8, "min_confidence": HAR_MODEL_MIN_CONFIDENCE, "classes": list(HAR_MODEL_CLASSES)}


def canonical_model_manifest(descriptor: dict[str, Any]) -> str:
    return json.dumps(descriptor, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def model_request_signature(token: str, method: str, path: str, session_id: uuid.UUID, device_id: str) -> str:
    material = f"v1|{method.upper()}|{path}|{session_id}|{device_id}".encode("utf-8")
    return hmac.new(token.encode("utf-8"), material, hashlib.sha256).hexdigest()


def validate_database_schema() -> None:
    required = {
        ("identification", "workers"): {"worker_id", "worker_uuid", "worker_name"},
        ("identification", "devices"): {"device_id", "ble_address"},
        ("public", "node_registrations"): {
            "registration_id", "device_id", "worker_id", "approval_status",
            "active", "current_session_id", "delinked_at",
        },
        ("monitoring", "sessions"): {
            "session_id", "device_id", "worker_id", "bearer_token_hash",
            "token_issued_at", "token_expires_at", "token_revoked_at",
            "start_time", "end_time", "active",
        },
        ("fall_detection", "activity_log"): {"session_id", "worker_id"},
        ("fall_detection", "classification"): {"session_id", "worker_id"},
    }
    with psycopg.connect(DATABASE_URL) as connection:
        for (schema, table), expected in required.items():
            rows = connection.execute(
                """
                SELECT column_name FROM information_schema.columns
                WHERE table_schema=%s AND table_name=%s
                """,
                (schema, table),
            ).fetchall()
            actual = {str(row[0]) for row in rows}
            missing = expected - actual
            if missing:
                raise RuntimeError(
                    f"{schema}.{table} missing columns {sorted(missing)}; apply database_schema.sql"
                )
    logger.info("Database schema is compatible with worker/session identity")


def sync_device(connection: psycopg.Connection[Any], device_id: str, ble_address: str | None) -> None:
    connection.execute(
        """
        INSERT INTO identification.devices(device_id, ble_address, registered_at, last_seen_at)
        VALUES (%s,%s,now(),now())
        ON CONFLICT(device_id) DO UPDATE SET
            ble_address=COALESCE(EXCLUDED.ble_address, identification.devices.ble_address),
            last_seen_at=now()
        """,
        (device_id, ble_address),
    )


def add_audit(
    connection: psycopg.Connection[Any],
    registration_id: int,
    device_id: str,
    worker_id: str | None,
    session_id: uuid.UUID | None,
    previous_status: str | None,
    new_status: str,
    actor: str,
    reason: str | None,
) -> None:
    connection.execute(
        """
        INSERT INTO identification.registration_audit(
            registration_id, device_id, worker_id, session_id,
            previous_status, new_status, actor, reason
        ) VALUES (%s,%s,%s,%s,%s,%s,%s,%s)
        """,
        (registration_id, device_id, worker_id, session_id, previous_status, new_status, actor, reason),
    )


def delink_registration(
    connection: psycopg.Connection[Any],
    registration_id: int,
    reason: str,
) -> None:
    row = connection.execute(
        """
        SELECT device_id, worker_id, approval_status, current_session_id
        FROM public.node_registrations
        WHERE registration_id=%s FOR UPDATE
        """,
        (registration_id,),
    ).fetchone()
    if row is None:
        return
    device_id, worker_id, previous_status, session_id = row
    if session_id is not None:
        connection.execute(
            """
            UPDATE monitoring.sessions
            SET active=FALSE,
                token_revoked_at=COALESCE(token_revoked_at, now()),
                end_time=COALESCE(end_time, now())
            WHERE session_id=%s AND active=TRUE
            """,
            (session_id,),
        )
    connection.execute(
        """
        UPDATE public.node_registrations
        SET active=FALSE,
            current_session_id=NULL,
            delinked_at=now(),
            decision_reason=COALESCE(decision_reason, %s)
        WHERE registration_id=%s
        """,
        (reason, registration_id),
    )
    add_audit(
        connection,
        registration_id,
        str(device_id),
        str(worker_id) if worker_id else None,
        session_id,
        str(previous_status),
        "rejected",
        "system",
        reason,
    )


def ensure_pending_registration(
    connection: psycopg.Connection[Any],
    device_id: str,
) -> int:
    row = connection.execute(
        """
        SELECT registration_id FROM public.node_registrations
        WHERE device_id=%s AND active=TRUE
        ORDER BY registration_id DESC LIMIT 1 FOR UPDATE
        """,
        (device_id,),
    ).fetchone()
    if row is not None:
        registration_id = int(row[0])
        connection.execute(
            """
            UPDATE public.node_registrations
            SET approval_status='pending', worker_id=NULL, current_session_id=NULL,
                requested_at=now(), decided_at=NULL, decided_by=NULL,
                decision_reason=NULL, last_seen_at=now()
            WHERE registration_id=%s
            """,
            (registration_id,),
        )
        return registration_id

    row = connection.execute(
        """
        INSERT INTO public.node_registrations(
            device_id, worker_id, approval_status, active,
            requested_at, registered_at, last_seen_at
        ) VALUES (%s,NULL,'pending',TRUE,now(),now(),now())
        RETURNING registration_id
        """,
        (device_id,),
    ).fetchone()
    registration_id = int(row[0])
    add_audit(
        connection, registration_id, device_id, None, None,
        None, "pending", "ble-registration", "new or delinked Nesso N1 requested assignment"
    )
    return registration_id


def register_or_verify(
    request: RegistrationRequest,
    server_uuid: uuid.UUID,
    ble_address: str | None,
) -> RegistrationResult:
    if request.expected_server_uuid and request.expected_server_uuid != server_uuid:
        return RegistrationResult(STATUS_SERVER_REJECTED, ZERO_UUID, server_uuid, "server_uuid_mismatch")

    now = datetime.now(timezone.utc)
    with psycopg.connect(DATABASE_URL) as connection:
        with connection.transaction():
            sync_device(connection, request.device_id, ble_address)
            row = connection.execute(
                """
                SELECT
                    nr.registration_id, nr.approval_status, nr.worker_id,
                    nr.current_session_id,
                    w.worker_uuid,
                    s.token_expires_at, s.token_revoked_at, s.active
                FROM public.node_registrations nr
                LEFT JOIN identification.workers w ON w.worker_id=nr.worker_id
                LEFT JOIN monitoring.sessions s ON s.session_id=nr.current_session_id
                WHERE nr.device_id=%s AND nr.active=TRUE
                ORDER BY nr.registration_id DESC
                LIMIT 1 FOR UPDATE OF nr
                """,
                (request.device_id,),
            ).fetchone()

            if row is None:
                ensure_pending_registration(connection, request.device_id)
                return RegistrationResult(STATUS_SERVER_REJECTED, ZERO_UUID, server_uuid, "registration_pending")

            registration_id, status, worker_id, session_id, worker_uuid, expires_at, revoked_at, session_active = row
            connection.execute(
                "UPDATE public.node_registrations SET last_seen_at=now() WHERE registration_id=%s",
                (registration_id,),
            )

            if status != "approved" or worker_id is None or worker_uuid is None or session_id is None:
                return RegistrationResult(STATUS_SERVER_REJECTED, ZERO_UUID, server_uuid, f"registration_{status}")

            expired = (
                expires_at is None
                or expires_at <= now
                or revoked_at is not None
                or not bool(session_active)
            )
            if expired:
                delink_registration(connection, int(registration_id), "session_expired")
                ensure_pending_registration(connection, request.device_id)
                return RegistrationResult(STATUS_SERVER_REJECTED, ZERO_UUID, server_uuid, "session_expired_delinked")

            assigned_uuid = uuid.UUID(str(worker_uuid))
            # A disposable node may legitimately hold a previous worker UUID.
            # The server returns the currently approved worker UUID and the node
            # replaces its stored assignment after approval.
            return RegistrationResult(
                STATUS_EXISTING_ACCEPTED if request.current_uuid == assigned_uuid else STATUS_NEW_ASSIGNED,
                assigned_uuid,
                server_uuid,
                "approved",
            )


def session_material_for_device(
    token_secret: bytes,
    device_id: str,
) -> dict[str, Any] | None:
    with psycopg.connect(DATABASE_URL) as connection:
        row = connection.execute(
            """
            SELECT
                s.session_id, s.device_id, s.worker_id,
                w.worker_uuid, w.worker_name,
                s.bearer_token_hash, s.token_issued_at, s.token_expires_at,
                s.token_revoked_at, s.active
            FROM public.node_registrations nr
            JOIN monitoring.sessions s ON s.session_id=nr.current_session_id
            JOIN identification.workers w ON w.worker_id=s.worker_id
            WHERE nr.device_id=%s
              AND nr.active=TRUE
              AND nr.approval_status='approved'
            """,
            (device_id,),
        ).fetchone()
    if row is None:
        return None
    (
        session_id, stored_device, worker_id, worker_uuid, worker_name,
        token_hash, issued_at, expires_at, revoked_at, active,
    ) = row
    now = datetime.now(timezone.utc)
    if not active or revoked_at is not None or expires_at <= now:
        return None
    token = derive_bearer_token(
        token_secret,
        uuid.UUID(str(session_id)),
        str(stored_device),
        str(worker_id),
        uuid.UUID(str(worker_uuid)),
        issued_at,
        expires_at,
    )
    if not hmac.compare_digest(hash_bearer_token(token), str(token_hash)):
        logger.error("Derived token hash mismatch for session %s", session_id)
        return None
    return {
        "session_id": uuid.UUID(str(session_id)),
        "device_id": str(stored_device),
        "worker_id": str(worker_id),
        "worker_uuid": uuid.UUID(str(worker_uuid)),
        "worker_name": str(worker_name),
        "token": token,
        "expires_at": expires_at,
    }


def resolve_worker(
    connection: psycopg.Connection[Any],
    worker_id: str | None,
    worker_name: str | None,
) -> tuple[str, uuid.UUID, str]:
    if worker_id:
        normalized_id = worker_id.strip().upper()
        row = connection.execute(
            "SELECT worker_id, worker_uuid, worker_name FROM identification.workers WHERE worker_id=%s",
            (normalized_id,),
        ).fetchone()
        if row is None:
            raise ValueError(f"worker_id {normalized_id} does not exist")
        return str(row[0]), uuid.UUID(str(row[1])), str(row[2])

    if worker_name is None:
        raise ValueError("approve requires worker_name or worker_id")
    name = normalize_worker_name(worker_name)
    row = connection.execute(
        """
        SELECT worker_id, worker_uuid, worker_name
        FROM identification.workers
        WHERE lower(worker_name)=lower(%s)
        FOR UPDATE
        """,
        (name,),
    ).fetchone()
    if row is not None:
        return str(row[0]), uuid.UUID(str(row[1])), str(row[2])

    new_id = generate_worker_id(connection)
    new_uuid = uuid.uuid4()
    connection.execute(
        """
        INSERT INTO identification.workers(worker_id, worker_uuid, worker_name)
        VALUES (%s,%s,%s)
        """,
        (new_id, new_uuid, name),
    )
    return new_id, new_uuid, name


def registration_payload(row: tuple[Any, ...]) -> dict[str, Any]:
    (
        registration_id, device_id, worker_id, worker_name, worker_uuid,
        approval_status, active, requested_at, registered_at, last_seen_at,
        decided_at, decided_by, decision_reason, current_session_id,
        token_issued_at, token_expires_at, token_revoked_at, delinked_at,
    ) = row
    iso = lambda value: value.isoformat() if value is not None else None
    return {
        "registration_id": int(registration_id),
        "device_id": str(device_id),
        "worker_id": str(worker_id) if worker_id else None,
        "worker_name": str(worker_name) if worker_name else None,
        "node_uuid": str(worker_uuid) if worker_uuid else None,
        "approval_status": str(approval_status),
        "active": bool(active),
        "requested_at": iso(requested_at),
        "registered_at": iso(registered_at),
        "last_seen_at": iso(last_seen_at),
        "decided_at": iso(decided_at),
        "decided_by": decided_by,
        "decision_reason": decision_reason,
        "session_id": str(current_session_id) if current_session_id else None,
        "token_issued_at": iso(token_issued_at),
        "token_expires_at": iso(token_expires_at),
        "token_revoked_at": iso(token_revoked_at),
        "delinked_at": iso(delinked_at),
    }


REGISTRATION_SELECT = """
SELECT
    nr.registration_id, nr.device_id, nr.worker_id, w.worker_name, w.worker_uuid,
    nr.approval_status, nr.active, nr.requested_at, nr.registered_at,
    nr.last_seen_at, nr.decided_at, nr.decided_by, nr.decision_reason,
    nr.current_session_id, s.token_issued_at, s.token_expires_at,
    s.token_revoked_at, nr.delinked_at
FROM public.node_registrations nr
LEFT JOIN identification.workers w ON w.worker_id=nr.worker_id
LEFT JOIN monitoring.sessions s ON s.session_id=nr.current_session_id
"""


class TervanixRegistrationServer:
    def __init__(self) -> None:
        validate_database_schema()
        descriptor = current_model_descriptor()
        logger.info("HAR model ready id=%s bytes=%d sha256=%s", descriptor["model_id"], descriptor["size"], descriptor["sha256"])
        self.server_uuid = load_server_uuid()
        self.token_secret = load_token_secret()
        self.ble: BlessServer | None = None
        self.registration_characteristic: BlessGATTCharacteristic | None = None
        self.credential_characteristic: BlessGATTCharacteristic | None = None
        self.worker_context_characteristic: BlessGATTCharacteristic | None = None
        self.worker_name_characteristic: BlessGATTCharacteristic | None = None
        self.central_devices: dict[str, str] = {}
        self.stop_event = asyncio.Event()
        self.ble_started = False
        self.admin_runner: web.AppRunner | None = None

    @staticmethod
    def _central_id(request: Any | None) -> str | None:
        if request is None:
            return None
        try:
            value = request.get("central_id")
        except (AttributeError, TypeError):
            value = getattr(request, "central_id", None)
        return str(value) if value else None

    def read_request(
        self,
        characteristic: BlessGATTCharacteristic,
        request: Any | None = None,
        **kwargs: Any,
    ) -> bytearray:
        del kwargs
        central_id = self._central_id(request)
        char_uuid = str(characteristic.uuid).lower()
        if char_uuid == REGISTRATION_UUID.lower():
            return characteristic.value
        if central_id is None:
            return bytearray()
        device_id = self.central_devices.get(central_id)
        if not device_id:
            logger.warning("Provisioning read from unknown central=%s", central_id)
            return bytearray()
        material = session_material_for_device(self.token_secret, device_id)
        if material is None:
            return bytearray()

        if char_uuid == CREDENTIAL_UUID.lower():
            return bytearray(material["token"].encode("ascii"))
        if char_uuid == WORKER_CONTEXT_UUID.lower():
            worker_id_bytes = material["worker_id"].encode("ascii")[:12].ljust(12, b"\0")
            expires_epoch = int(material["expires_at"].timestamp())
            return bytearray(struct.pack(
                WORKER_CONTEXT_FORMAT,
                WORKER_CONTEXT_VERSION,
                worker_id_bytes,
                material["session_id"].bytes,
                expires_epoch,
            ))
        if char_uuid == WORKER_NAME_UUID.lower():
            return bytearray(material["worker_name"].encode("utf-8"))
        return bytearray()

    def write_request(
        self,
        characteristic: BlessGATTCharacteristic,
        value: bytes | bytearray,
        request: Any | None = None,
        **kwargs: Any,
    ) -> None:
        del kwargs
        central_id = self._central_id(request)
        if str(characteristic.uuid).lower() != REGISTRATION_UUID.lower():
            return
        registration: RegistrationRequest | None = None
        try:
            registration = parse_registration_request(bytes(value))
            if central_id:
                self.central_devices[central_id] = registration.device_id
            logger.info(
                "BLE registration request central=%s device_id=%s current_uuid=%s expected_server_uuid=%s nonce=%u",
                central_id, registration.device_id, registration.current_uuid,
                registration.expected_server_uuid, registration.nonce,
            )
            result = register_or_verify(registration, self.server_uuid, central_id)
            characteristic.value = bytearray(build_registration_response(result, registration.nonce))
            logger.info(
                "BLE registration response central=%s status=%d reason=%s node_uuid=%s nonce=%u",
                central_id, result.status, result.reason, result.node_uuid, registration.nonce,
            )
        except Exception:
            logger.exception("BLE registration failed central=%s", central_id)
            result = RegistrationResult(STATUS_ERROR, ZERO_UUID, self.server_uuid, "invalid_request")
            characteristic.value = bytearray(
                build_registration_response(result, registration.nonce if registration else 0)
            )

    async def configure_ble(self) -> None:
        self.ble = BlessServer(name=SERVER_NAME, loop=asyncio.get_running_loop())
        self.ble.read_request_func = self.read_request
        self.ble.write_request_func = self.write_request
        await self.ble.add_new_service(SERVICE_UUID)
        await self.ble.add_new_characteristic(
            SERVICE_UUID, REGISTRATION_UUID,
            GATTCharacteristicProperties.read | GATTCharacteristicProperties.write,
            bytearray(RESPONSE_SIZE),
            GATTAttributePermissions.readable | GATTAttributePermissions.writable,
        )
        for char_uuid in (CREDENTIAL_UUID, WORKER_CONTEXT_UUID, WORKER_NAME_UUID):
            await self.ble.add_new_characteristic(
                SERVICE_UUID, char_uuid, GATTCharacteristicProperties.read,
                bytearray(), GATTAttributePermissions.readable,
            )
        self.registration_characteristic = self.ble.get_characteristic(REGISTRATION_UUID)
        self.credential_characteristic = self.ble.get_characteristic(CREDENTIAL_UUID)
        self.worker_context_characteristic = self.ble.get_characteristic(WORKER_CONTEXT_UUID)
        self.worker_name_characteristic = self.ble.get_characteristic(WORKER_NAME_UUID)
        if not all((
            self.registration_characteristic,
            self.credential_characteristic,
            self.worker_context_characteristic,
            self.worker_name_characteristic,
        )):
            raise RuntimeError("BLE registration characteristics were not created")

    def _require_admin(self, request: web.Request) -> None:
        if ADMIN_TOKEN:
            expected = f"Bearer {ADMIN_TOKEN}"
            if not hmac.compare_digest(request.headers.get("Authorization", ""), expected):
                raise web.HTTPUnauthorized(text=json.dumps({"error": "invalid_admin_token"}), content_type="application/json")
            return
        remote = request.remote
        try:
            loopback = bool(remote and ipaddress.ip_address(remote).is_loopback)
        except ValueError:
            loopback = False
        if not loopback:
            raise web.HTTPForbidden(
                text=json.dumps({"error": "admin_api_is_local_only_without_token"}),
                content_type="application/json",
            )

    async def health(self, request: web.Request) -> web.Response:
        del request
        with psycopg.connect(DATABASE_URL) as connection:
            counts = connection.execute(
                "SELECT approval_status,count(*) FROM public.node_registrations WHERE active=TRUE GROUP BY approval_status"
            ).fetchall()
            workers = connection.execute(
                "SELECT count(*) FROM identification.workers WHERE active=TRUE"
            ).fetchone()[0]
            sessions = connection.execute(
                "SELECT count(*) FROM monitoring.sessions WHERE active=TRUE"
            ).fetchone()[0]
        return web.json_response({
            "status": "ok",
            "server_uuid": str(self.server_uuid),
            "ble": self.ble_started,
            "admin_mode": "token" if ADMIN_TOKEN else "local-only",
            "session_ttl_seconds": SESSION_TTL_SECONDS,
            "active_workers": int(workers),
            "active_sessions": int(sessions),
            "registrations": {str(status): int(count) for status, count in counts},
        })

    async def list_workers(self, request: web.Request) -> web.Response:
        self._require_admin(request)
        with psycopg.connect(DATABASE_URL) as connection:
            rows = connection.execute(
                """
                SELECT worker_id, worker_uuid, worker_name, active, created_at, updated_at
                FROM identification.workers
                ORDER BY lower(worker_name), worker_id
                """
            ).fetchall()
        return web.json_response({"workers": [
            {
                "worker_id": str(worker_id),
                "worker_uuid": str(worker_uuid),
                "worker_name": str(worker_name),
                "active": bool(active),
                "created_at": created_at.isoformat(),
                "updated_at": updated_at.isoformat(),
            }
            for worker_id, worker_uuid, worker_name, active, created_at, updated_at in rows
        ]})

    async def list_nodes(self, request: web.Request) -> web.Response:
        self._require_admin(request)
        status = request.query.get("status", "pending").lower()
        if status not in {*VALID_APPROVAL_STATUSES, "all"}:
            raise web.HTTPBadRequest(text=json.dumps({"error": "invalid_status"}), content_type="application/json")
        sql = REGISTRATION_SELECT + " WHERE nr.active=TRUE"
        params: tuple[Any, ...] = ()
        if status != "all":
            sql += " AND nr.approval_status=%s"
            params = (status,)
        sql += " ORDER BY nr.requested_at DESC, nr.registration_id DESC"
        with psycopg.connect(DATABASE_URL) as connection:
            rows = connection.execute(sql, params).fetchall()
        return web.json_response({"nodes": [registration_payload(row) for row in rows]})

    async def show_node(self, request: web.Request) -> web.Response:
        self._require_admin(request)
        try:
            device_id = normalize_device_id(request.match_info["device_id"])
        except ValueError as error:
            raise web.HTTPBadRequest(text=str(error)) from error
        with psycopg.connect(DATABASE_URL) as connection:
            row = connection.execute(
                REGISTRATION_SELECT + " WHERE nr.device_id=%s ORDER BY nr.registration_id DESC LIMIT 1",
                (device_id,),
            ).fetchone()
            if row is None:
                raise web.HTTPNotFound(text=json.dumps({"error": "registration_not_found"}), content_type="application/json")
            audit = connection.execute(
                """
                SELECT audit_id, worker_id, session_id, previous_status, new_status, actor, reason, created_at
                FROM identification.registration_audit
                WHERE registration_id=%s ORDER BY created_at,audit_id
                """,
                (row[0],),
            ).fetchall()
        payload = registration_payload(row)
        payload["audit"] = [
            {
                "audit_id": int(aid), "worker_id": wid,
                "session_id": str(sid) if sid else None,
                "previous_status": prev, "new_status": new,
                "actor": actor, "reason": reason, "created_at": created.isoformat(),
            }
            for aid, wid, sid, prev, new, actor, reason, created in audit
        ]
        return web.json_response(payload)

    async def decide_node(self, request: web.Request) -> web.Response:
        self._require_admin(request)
        try:
            device_id = normalize_device_id(request.match_info["device_id"])
        except ValueError as error:
            raise web.HTTPBadRequest(text=str(error)) from error
        decision = request.match_info["decision"].lower()
        if decision not in {"approve", "reject"}:
            raise web.HTTPNotFound()
        try:
            body = await request.json()
        except (json.JSONDecodeError, web.HTTPBadRequest):
            body = {}
        actor = str(body.get("actor") or "admin")[:200]
        reason = str(body.get("reason") or "")[:1000] or None

        with psycopg.connect(DATABASE_URL) as connection:
            with connection.transaction():
                row = connection.execute(
                    """
                    SELECT registration_id, approval_status, worker_id, current_session_id
                    FROM public.node_registrations
                    WHERE device_id=%s AND active=TRUE
                    ORDER BY registration_id DESC LIMIT 1 FOR UPDATE
                    """,
                    (device_id,),
                ).fetchone()
                if row is None:
                    raise web.HTTPNotFound(text=json.dumps({"error": "registration_not_found"}), content_type="application/json")
                registration_id, previous_status, previous_worker, previous_session = row

                if decision == "reject":
                    if previous_session:
                        connection.execute(
                            """
                            UPDATE monitoring.sessions SET active=FALSE,
                                token_revoked_at=COALESCE(token_revoked_at,now()),
                                end_time=COALESCE(end_time,now())
                            WHERE session_id=%s
                            """,
                            (previous_session,),
                        )
                    connection.execute(
                        """
                        UPDATE public.node_registrations
                        SET approval_status='rejected', worker_id=NULL,
                            current_session_id=NULL, decided_at=now(),
                            decided_by=%s, decision_reason=%s
                        WHERE registration_id=%s
                        """,
                        (actor, reason, registration_id),
                    )
                    add_audit(
                        connection, int(registration_id), device_id,
                        str(previous_worker) if previous_worker else None,
                        previous_session, str(previous_status), "rejected", actor, reason,
                    )
                else:
                    try:
                        worker_id, worker_uuid, worker_name = resolve_worker(
                            connection,
                            str(body.get("worker_id") or "") or None,
                            str(body.get("worker_name") or "") or None,
                        )
                    except ValueError as error:
                        raise web.HTTPBadRequest(
                            text=json.dumps({"error": str(error)}), content_type="application/json"
                        ) from error

                    # A worker identity can have one live disposable Nesso at a time.
                    old_sessions = connection.execute(
                        """
                        SELECT session_id, device_id FROM monitoring.sessions
                        WHERE active=TRUE AND (worker_id=%s OR device_id=%s)
                        FOR UPDATE
                        """,
                        (worker_id, device_id),
                    ).fetchall()
                    for old_session_id, old_device_id in old_sessions:
                        connection.execute(
                            """
                            UPDATE monitoring.sessions
                            SET active=FALSE, token_revoked_at=now(), end_time=now()
                            WHERE session_id=%s
                            """,
                            (old_session_id,),
                        )
                        connection.execute(
                            """
                            UPDATE public.node_registrations
                            SET active=FALSE, current_session_id=NULL, delinked_at=now()
                            WHERE current_session_id=%s
                              AND registration_id<>%s
                            """,
                            (old_session_id, registration_id),
                        )
                        if str(old_device_id) == device_id and previous_session == old_session_id:
                            previous_session = None

                    session_id = uuid.uuid4()
                    issued_at = datetime.now(timezone.utc)
                    expires_at = issued_at + timedelta(seconds=SESSION_TTL_SECONDS)
                    token = derive_bearer_token(
                        self.token_secret, session_id, device_id, worker_id,
                        worker_uuid, issued_at, expires_at,
                    )
                    token_hash = hash_bearer_token(token)
                    connection.execute(
                        """
                        INSERT INTO monitoring.sessions(
                            session_id, device_id, worker_id, bearer_token_hash,
                            token_issued_at, token_expires_at, start_time,
                            sample_rate_hz, active
                        ) VALUES (%s,%s,%s,%s,%s,%s,now(),%s,TRUE)
                        """,
                        (
                            session_id, device_id, worker_id, token_hash,
                            issued_at, expires_at, SAMPLE_RATE_HZ,
                        ),
                    )
                    connection.execute(
                        """
                        UPDATE public.node_registrations
                        SET worker_id=%s, approval_status='approved', active=TRUE,
                            current_session_id=%s, decided_at=now(), decided_by=%s,
                            decision_reason=%s, delinked_at=NULL, last_seen_at=now()
                        WHERE registration_id=%s
                        """,
                        (worker_id, session_id, actor, reason, registration_id),
                    )
                    add_audit(
                        connection, int(registration_id), device_id, worker_id,
                        session_id, str(previous_status), "approved", actor, reason,
                    )
                    logger.info(
                        "Approved device=%s worker=%s/%s worker_uuid=%s session=%s expires=%s",
                        device_id, worker_id, worker_name, worker_uuid, session_id, expires_at,
                    )

                updated = connection.execute(
                    REGISTRATION_SELECT + " WHERE nr.registration_id=%s",
                    (registration_id,),
                ).fetchone()
        return web.json_response(registration_payload(updated))

    def _require_model_session(self, request: web.Request) -> dict[str, Any]:
        raw_session = request.headers.get("X-Tervanix-Session", "").strip()
        raw_device = request.headers.get("X-Tervanix-Device", "").strip()
        supplied = request.headers.get("X-Tervanix-Auth", "").strip().lower()
        try:
            session_id = uuid.UUID(raw_session)
            device_id = normalize_device_id(raw_device)
        except ValueError as error:
            raise web.HTTPUnauthorized(text=json.dumps({"error":"invalid_model_session_headers"}), content_type="application/json") from error
        material = session_material_for_device(self.token_secret, device_id)
        if material is None or material["session_id"] != session_id:
            raise web.HTTPUnauthorized(text=json.dumps({"error":"model_session_not_active"}), content_type="application/json")
        expected = model_request_signature(material["token"], request.method, request.path, session_id, device_id)
        if not hmac.compare_digest(supplied, expected):
            raise web.HTTPUnauthorized(text=json.dumps({"error":"invalid_model_request_signature"}), content_type="application/json")
        return material

    async def model_manifest(self, request: web.Request) -> web.Response:
        material = self._require_model_session(request)
        descriptor = current_model_descriptor()
        payload = canonical_model_manifest(descriptor)
        mac = hmac.new(material["token"].encode("utf-8"), payload.encode("utf-8"), hashlib.sha256).hexdigest()
        return web.json_response({"payload": payload, "hmac_sha256": mac})

    async def model_binary(self, request: web.Request) -> web.StreamResponse:
        self._require_model_session(request)
        descriptor = current_model_descriptor()
        response = web.FileResponse(HAR_MODEL_PATH)
        response.headers["Content-Type"] = "application/octet-stream"
        response.headers["X-Tervanix-Model-Id"] = str(descriptor["model_id"])
        response.headers["X-Tervanix-Model-SHA256"] = str(descriptor["sha256"])
        response.headers["Cache-Control"] = "no-store"
        return response

    async def start_admin_api(self) -> None:
        app = web.Application()
        app.router.add_get("/health", self.health)
        app.router.add_get("/v1/device/model/manifest", self.model_manifest)
        app.router.add_get("/v1/device/model/binary", self.model_binary)
        app.router.add_get("/v1/admin/workers", self.list_workers)
        app.router.add_get("/v1/admin/nodes", self.list_nodes)
        app.router.add_get("/v1/admin/nodes/{device_id}", self.show_node)
        app.router.add_post(
            "/v1/admin/nodes/{device_id}/{decision:approve|reject}", self.decide_node
        )
        self.admin_runner = web.AppRunner(app)
        await self.admin_runner.setup()
        await web.TCPSite(self.admin_runner, ADMIN_HOST, ADMIN_PORT).start()
        logger.info(
            "Admin API listening on %s:%d; session_ttl=%ds; admin_mode=%s",
            ADMIN_HOST, ADMIN_PORT, SESSION_TTL_SECONDS,
            "token" if ADMIN_TOKEN else "local-only",
        )

    async def run(self) -> None:
        await self.start_admin_api()
        await self.configure_ble()
        if self.ble is None:
            raise RuntimeError("BLE server is not configured")
        try:
            await self.ble.start()
            self.ble_started = True
            logger.info(
                "BLE registration server started as %s; request=%d response=%d context=%d",
                SERVER_NAME, REQUEST_SIZE, RESPONSE_SIZE, WORKER_CONTEXT_SIZE,
            )
            await self.stop_event.wait()
        finally:
            if self.ble_started:
                await self.ble.stop()
            if self.admin_runner:
                await self.admin_runner.cleanup()

    def stop(self) -> None:
        self.stop_event.set()


async def async_main() -> None:
    application = TervanixRegistrationServer()
    loop = asyncio.get_running_loop()
    for received_signal in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(received_signal, application.stop)
        except NotImplementedError:
            pass
    await application.run()


def main() -> None:
    try:
        asyncio.run(async_main())
    except DBusError as error:
        logger.error("BlueZ error: %s", error)
        raise SystemExit(1) from error
    except KeyboardInterrupt:
        logger.info("Stopped")
    except Exception:
        logger.exception("Tervanix registration server failed")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
