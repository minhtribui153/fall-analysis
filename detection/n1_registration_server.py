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
from datetime import datetime, timezone
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

PROTOCOL_VERSION = 2
FLAG_HAS_UUID = 1 << 0
FLAG_HAS_SERVER_UUID = 1 << 1

STATUS_ERROR = 0
STATUS_EXISTING_ACCEPTED = 1
STATUS_NEW_ASSIGNED = 2
STATUS_IDENTITY_REJECTED = 3
STATUS_SERVER_REJECTED = 4

# These formats intentionally match the immutable C++ structs. The response
# contains three alignment bytes before uint32_t nonce and is therefore 40 B.
REQUEST_FORMAT = "<BB6s16s16sI"
RESPONSE_FORMAT = "<B16s16s3xI"
REQUEST_SIZE = struct.calcsize(REQUEST_FORMAT)
RESPONSE_SIZE = struct.calcsize(RESPONSE_FORMAT)
ZERO_UUID = uuid.UUID(int=0)

ADMIN_HOST = os.getenv("REGISTRATION_ADMIN_HOST", "0.0.0.0")
ADMIN_PORT = int(os.getenv("REGISTRATION_ADMIN_PORT", "8765"))
ADMIN_TOKEN = os.getenv("REGISTRATION_ADMIN_TOKEN", "").strip()

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


def parse_registration_request(value: bytes) -> RegistrationRequest:
    if len(value) != REQUEST_SIZE:
        raise ValueError(
            f"Expected {REQUEST_SIZE} registration bytes, received {len(value)}"
        )

    (
        version,
        flags,
        device_id_bytes,
        current_uuid_raw,
        expected_server_uuid_raw,
        nonce,
    ) = struct.unpack(REQUEST_FORMAT, value)

    if version != PROTOCOL_VERSION:
        raise ValueError(f"Unsupported registration protocol version {version}")

    current_uuid: uuid.UUID | None = None
    if flags & FLAG_HAS_UUID:
        parsed = uuid.UUID(bytes=current_uuid_raw)
        if parsed.int != 0:
            current_uuid = parsed

    expected_server_uuid: uuid.UUID | None = None
    if flags & FLAG_HAS_SERVER_UUID:
        parsed = uuid.UUID(bytes=expected_server_uuid_raw)
        if parsed.int != 0:
            expected_server_uuid = parsed

    return RegistrationRequest(
        device_id_bytes=device_id_bytes,
        device_id=format_device_id(device_id_bytes),
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
        value = uuid.UUID(configured)
        logger.info("Using REGISTRATION_SERVER_UUID=%s", value)
        return value

    default_file = Path(__file__).resolve().with_name(".registration_server_uuid")
    path = Path(os.getenv("REGISTRATION_SERVER_UUID_FILE", str(default_file)))

    if path.exists():
        value = uuid.UUID(path.read_text(encoding="utf-8").strip())
        logger.info("Loaded registration server UUID %s from %s", value, path)
        return value

    value = uuid.uuid4()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(str(value) + "\n", encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass
    logger.info("Created registration server UUID %s in %s", value, path)
    return value


def load_token_secret() -> bytes:
    configured = os.getenv("REGISTRATION_TOKEN_SECRET", "").strip()
    if configured:
        if len(configured) < 32:
            raise RuntimeError("REGISTRATION_TOKEN_SECRET must be at least 32 characters")
        return configured.encode("utf-8")

    default_file = Path(__file__).resolve().with_name(".registration_token_secret")
    path = Path(os.getenv("REGISTRATION_TOKEN_SECRET_FILE", str(default_file)))

    if path.exists():
        value = path.read_text(encoding="utf-8").strip()
        if len(value) < 32:
            raise RuntimeError(f"Token secret in {path} is too short")
        logger.info("Loaded persistent registration token secret from %s", path)
        return value.encode("utf-8")

    value = secrets.token_urlsafe(48)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(value + "\n", encoding="utf-8")
    try:
        path.chmod(0o600)
    except OSError:
        pass
    logger.info("Created persistent registration token secret in %s", path)
    return value.encode("utf-8")


def derive_bearer_token(
    token_secret: bytes,
    registration_id: int,
    device_id: str,
    node_uuid: uuid.UUID,
    token_issued_at: datetime,
) -> str:
    if token_issued_at.tzinfo is None:
        token_issued_at = token_issued_at.replace(tzinfo=timezone.utc)
    issued_us = int(token_issued_at.timestamp() * 1_000_000)
    material = (
        f"v1|{registration_id}|{device_id}|{node_uuid}|{issued_us}"
    ).encode("utf-8")
    digest = hmac.new(token_secret, material, hashlib.sha256).digest()
    return base64.urlsafe_b64encode(digest).rstrip(b"=").decode("ascii")


def hash_bearer_token(token: str) -> str:
    return hashlib.sha256(token.encode("utf-8")).hexdigest()


def validate_database_schema() -> None:
    required = {
        ("public", "node_registrations"): {
            "registration_id",
            "device_id",
            "node_uuid",
            "approval_status",
            "token_hash",
            "token_issued_at",
            "token_revoked_at",
            "active",
            "requested_at",
            "registered_at",
            "last_seen_at",
            "decided_at",
            "decided_by",
            "decision_reason",
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
        ("devices", "registration_audit"): {
            "audit_id",
            "registration_id",
            "device_id",
            "previous_status",
            "new_status",
            "actor",
            "reason",
            "created_at",
        },
    }

    with psycopg.connect(DATABASE_URL) as connection:
        for (schema_name, table_name), expected_columns in required.items():
            rows = connection.execute(
                """
                SELECT column_name
                FROM information_schema.columns
                WHERE table_schema = %s
                  AND table_name = %s
                """,
                (schema_name, table_name),
            ).fetchall()
            actual_columns = {str(row[0]) for row in rows}
            missing = sorted(expected_columns - actual_columns)
            if missing:
                raise RuntimeError(
                    f"{schema_name}.{table_name} is missing columns: "
                    + ", ".join(missing)
                )

        obsolete_node_uuid = connection.execute(
            """
            SELECT EXISTS (
                SELECT 1
                FROM information_schema.columns
                WHERE table_schema = 'devices'
                  AND table_name = 'identification'
                  AND column_name = 'node_uuid'
            )
            """
        ).fetchone()[0]
        if obsolete_node_uuid:
            raise RuntimeError(
                "devices.identification still contains node_uuid; apply the current "
                "device_id-only schema before starting the registration server"
            )

    logger.info("Database schema is compatible with registration + bearer auth")


def sync_identification(
    connection: psycopg.Connection[Any],
    device_id: str,
    ble_address: str | None,
) -> None:
    connection.execute(
        """
        INSERT INTO devices.identification (
            device_id,
            ble_address,
            registered_at,
            last_seen_at
        )
        VALUES (%s, %s, now(), now())
        ON CONFLICT (device_id)
        DO UPDATE SET
            ble_address = COALESCE(
                EXCLUDED.ble_address,
                devices.identification.ble_address
            ),
            last_seen_at = now()
        """,
        (device_id, ble_address),
    )


def add_audit(
    connection: psycopg.Connection[Any],
    registration_id: int,
    device_id: str,
    previous_status: str | None,
    new_status: str,
    actor: str,
    reason: str | None,
) -> None:
    connection.execute(
        """
        INSERT INTO devices.registration_audit (
            registration_id,
            device_id,
            previous_status,
            new_status,
            actor,
            reason
        )
        VALUES (%s, %s, %s, %s, %s, %s)
        """,
        (
            registration_id,
            device_id,
            previous_status,
            new_status,
            actor,
            reason or None,
        ),
    )


def register_or_verify(
    request: RegistrationRequest,
    server_uuid: uuid.UUID,
    ble_address: str | None,
) -> RegistrationResult:
    if (
        request.expected_server_uuid is not None
        and request.expected_server_uuid != server_uuid
    ):
        return RegistrationResult(
            STATUS_SERVER_REJECTED,
            ZERO_UUID,
            server_uuid,
            "server_uuid_mismatch",
        )

    with psycopg.connect(DATABASE_URL) as connection:
        with connection.transaction():
            active_row = connection.execute(
                """
                SELECT
                    registration_id,
                    node_uuid,
                    approval_status,
                    token_hash,
                    token_issued_at
                FROM public.node_registrations
                WHERE device_id = %s
                  AND active = TRUE
                FOR UPDATE
                """,
                (request.device_id,),
            ).fetchone()

            if active_row is not None:
                (
                    registration_id,
                    registered_uuid,
                    approval_status,
                    token_hash,
                    token_issued_at,
                ) = active_row

                if (
                    request.current_uuid is not None
                    and request.current_uuid != registered_uuid
                ):
                    return RegistrationResult(
                        STATUS_IDENTITY_REJECTED,
                        ZERO_UUID,
                        server_uuid,
                        "stored_uuid_does_not_match_device",
                    )

                connection.execute(
                    """
                    UPDATE public.node_registrations
                    SET last_seen_at = now()
                    WHERE registration_id = %s
                    """,
                    (registration_id,),
                )
                sync_identification(connection, request.device_id, ble_address)

                if approval_status != "approved":
                    return RegistrationResult(
                        STATUS_SERVER_REJECTED,
                        ZERO_UUID,
                        server_uuid,
                        f"registration_{approval_status}",
                    )

                if token_hash is None or token_issued_at is None:
                    return RegistrationResult(
                        STATUS_ERROR,
                        ZERO_UUID,
                        server_uuid,
                        "approved_registration_missing_token",
                    )

                return RegistrationResult(
                    STATUS_EXISTING_ACCEPTED
                    if request.current_uuid is not None
                    else STATUS_NEW_ASSIGNED,
                    registered_uuid,
                    server_uuid,
                    "approved",
                )

            assigned_uuid = request.current_uuid or uuid.uuid4()

            existing_uuid_row = connection.execute(
                """
                SELECT registration_id, device_id, active
                FROM public.node_registrations
                WHERE node_uuid = %s
                FOR UPDATE
                """,
                (assigned_uuid,),
            ).fetchone()

            if existing_uuid_row is not None:
                registration_id, owner_device_id, owner_active = existing_uuid_row
                if owner_device_id != request.device_id:
                    return RegistrationResult(
                        STATUS_IDENTITY_REJECTED,
                        ZERO_UUID,
                        server_uuid,
                        "stored_uuid_belongs_to_other_device",
                    )
                if owner_active:
                    return RegistrationResult(
                        STATUS_ERROR,
                        ZERO_UUID,
                        server_uuid,
                        "registration_state_inconsistent",
                    )

                connection.execute(
                    """
                    UPDATE public.node_registrations
                    SET active = TRUE,
                        approval_status = 'pending',
                        token_hash = NULL,
                        token_issued_at = NULL,
                        token_revoked_at = NULL,
                        requested_at = now(),
                        last_seen_at = now(),
                        decided_at = NULL,
                        decided_by = NULL,
                        decision_reason = NULL,
                        delinked_at = NULL
                    WHERE registration_id = %s
                    """,
                    (registration_id,),
                )
                add_audit(
                    connection,
                    int(registration_id),
                    request.device_id,
                    None,
                    "pending",
                    "ble-registration",
                    "inactive registration reactivated",
                )
            else:
                row = connection.execute(
                    """
                    INSERT INTO public.node_registrations (
                        device_id,
                        node_uuid,
                        approval_status,
                        active,
                        requested_at,
                        registered_at,
                        last_seen_at
                    )
                    VALUES (%s, %s, 'pending', TRUE, now(), now(), now())
                    RETURNING registration_id
                    """,
                    (request.device_id, assigned_uuid),
                ).fetchone()
                registration_id = int(row[0])
                add_audit(
                    connection,
                    registration_id,
                    request.device_id,
                    None,
                    "pending",
                    "ble-registration",
                    "new BLE registration request",
                )

            sync_identification(connection, request.device_id, ble_address)

            return RegistrationResult(
                STATUS_SERVER_REJECTED,
                ZERO_UUID,
                server_uuid,
                "registration_pending",
            )


def token_for_device(token_secret: bytes, device_id: str) -> str | None:
    with psycopg.connect(DATABASE_URL) as connection:
        row = connection.execute(
            """
            SELECT
                registration_id,
                node_uuid,
                approval_status,
                token_hash,
                token_issued_at
            FROM public.node_registrations
            WHERE device_id = %s
              AND active = TRUE
            """,
            (device_id,),
        ).fetchone()

    if row is None:
        return None

    registration_id, node_uuid, status, token_hash, token_issued_at = row
    if status != "approved" or token_hash is None or token_issued_at is None:
        return None

    token = derive_bearer_token(
        token_secret,
        int(registration_id),
        device_id,
        node_uuid,
        token_issued_at,
    )
    if not hmac.compare_digest(hash_bearer_token(token), str(token_hash)):
        logger.error("Stored token hash does not match derived token for %s", device_id)
        return None
    return token


def registration_payload(row: tuple[Any, ...]) -> dict[str, Any]:
    (
        registration_id,
        device_id,
        node_uuid,
        approval_status,
        active,
        requested_at,
        registered_at,
        last_seen_at,
        decided_at,
        decided_by,
        decision_reason,
        token_hash,
        token_issued_at,
        token_revoked_at,
        last_authenticated_at,
        last_auth_failure_at,
        delinked_at,
    ) = row

    def iso(value: Any) -> str | None:
        return value.isoformat() if value is not None else None

    return {
        "registration_id": int(registration_id),
        "device_id": str(device_id),
        "node_uuid": str(node_uuid),
        "approval_status": str(approval_status),
        "active": bool(active),
        "token_issued": token_hash is not None,
        "requested_at": iso(requested_at),
        "registered_at": iso(registered_at),
        "last_seen_at": iso(last_seen_at),
        "decided_at": iso(decided_at),
        "decided_by": decided_by,
        "decision_reason": decision_reason,
        "token_issued_at": iso(token_issued_at),
        "token_revoked_at": iso(token_revoked_at),
        "last_authenticated_at": iso(last_authenticated_at),
        "last_auth_failure_at": iso(last_auth_failure_at),
        "delinked_at": iso(delinked_at),
    }


REGISTRATION_SELECT = """
SELECT
    registration_id,
    device_id,
    node_uuid,
    approval_status,
    active,
    requested_at,
    registered_at,
    last_seen_at,
    decided_at,
    decided_by,
    decision_reason,
    token_hash,
    token_issued_at,
    token_revoked_at,
    last_authenticated_at,
    last_auth_failure_at,
    delinked_at
FROM public.node_registrations
"""


class TervanixRegistrationServer:
    def __init__(self) -> None:
        validate_database_schema()
        self.server_uuid = load_server_uuid()
        self.token_secret = load_token_secret()
        self.ble: BlessServer | None = None
        self.registration_characteristic: BlessGATTCharacteristic | None = None
        self.credential_characteristic: BlessGATTCharacteristic | None = None
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
        characteristic_uuid = str(characteristic.uuid).lower()

        if characteristic_uuid == CREDENTIAL_UUID.lower():
            if central_id is None:
                logger.warning("Credential read without central_id")
                return bytearray()

            device_id = self.central_devices.get(central_id)
            if device_id is None:
                logger.warning("Credential read from unregistered central=%s", central_id)
                return bytearray()

            token = token_for_device(self.token_secret, device_id)
            if token is None:
                logger.info(
                    "Credential unavailable central=%s device_id=%s",
                    central_id,
                    device_id,
                )
                return bytearray()

            logger.info(
                "Provisioning MQTT bearer token central=%s device_id=%s bytes=%d",
                central_id,
                device_id,
                len(token),
            )
            return bytearray(token.encode("ascii"))

        logger.debug(
            "BLE read central=%s characteristic=%s bytes=%d",
            central_id,
            characteristic.uuid,
            len(characteristic.value),
        )
        return characteristic.value

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
            logger.warning(
                "Ignoring BLE write central=%s characteristic=%s",
                central_id,
                characteristic.uuid,
            )
            return

        registration: RegistrationRequest | None = None
        try:
            registration = parse_registration_request(bytes(value))
            if central_id is not None:
                self.central_devices[central_id] = registration.device_id

            logger.info(
                "BLE registration request central=%s device_id=%s "
                "current_uuid=%s expected_server_uuid=%s nonce=%u",
                central_id,
                registration.device_id,
                registration.current_uuid,
                registration.expected_server_uuid,
                registration.nonce,
            )

            result = register_or_verify(
                registration,
                self.server_uuid,
                central_id,
            )
            characteristic.value = bytearray(
                build_registration_response(result, registration.nonce)
            )

            logger.info(
                "BLE registration response central=%s status=%d reason=%s "
                "node_uuid=%s server_uuid=%s nonce=%u bytes=%d",
                central_id,
                result.status,
                result.reason,
                result.node_uuid,
                result.server_uuid,
                registration.nonce,
                RESPONSE_SIZE,
            )
        except Exception:
            logger.exception("BLE registration failed central=%s", central_id)
            result = RegistrationResult(
                STATUS_ERROR,
                ZERO_UUID,
                self.server_uuid,
                "invalid_request",
            )
            characteristic.value = bytearray(
                build_registration_response(
                    result,
                    registration.nonce if registration else 0,
                )
            )

    async def configure_ble(self) -> None:
        self.ble = BlessServer(name=SERVER_NAME, loop=asyncio.get_running_loop())
        self.ble.read_request_func = self.read_request
        self.ble.write_request_func = self.write_request

        await self.ble.add_new_service(SERVICE_UUID)
        await self.ble.add_new_characteristic(
            SERVICE_UUID,
            REGISTRATION_UUID,
            GATTCharacteristicProperties.read | GATTCharacteristicProperties.write,
            bytearray(RESPONSE_SIZE),
            GATTAttributePermissions.readable | GATTAttributePermissions.writable,
        )
        await self.ble.add_new_characteristic(
            SERVICE_UUID,
            CREDENTIAL_UUID,
            GATTCharacteristicProperties.read,
            bytearray(),
            GATTAttributePermissions.readable,
        )

        self.registration_characteristic = self.ble.get_characteristic(
            REGISTRATION_UUID
        )
        self.credential_characteristic = self.ble.get_characteristic(CREDENTIAL_UUID)
        if self.registration_characteristic is None or self.credential_characteristic is None:
            raise RuntimeError("Registration/credential characteristic was not created")

    def _require_admin(self, request: web.Request) -> None:
        if ADMIN_TOKEN:
            authorization = request.headers.get("Authorization", "")
            expected = f"Bearer {ADMIN_TOKEN}"
            if not hmac.compare_digest(authorization, expected):
                raise web.HTTPUnauthorized(
                    text=json.dumps({"error": "invalid_admin_token"}),
                    content_type="application/json",
                )
            return

        remote = request.remote
        try:
            is_loopback = bool(remote and ipaddress.ip_address(remote).is_loopback)
        except ValueError:
            is_loopback = False
        if not is_loopback:
            raise web.HTTPForbidden(
                text=json.dumps(
                    {"error": "admin_api_is_local_only_without_token"}
                ),
                content_type="application/json",
            )

    async def health(self, request: web.Request) -> web.Response:
        del request
        with psycopg.connect(DATABASE_URL) as connection:
            counts = connection.execute(
                """
                SELECT approval_status, count(*)
                FROM public.node_registrations
                WHERE active = TRUE
                GROUP BY approval_status
                """
            ).fetchall()
        return web.json_response(
            {
                "status": "ok",
                "server_uuid": str(self.server_uuid),
                "ble": self.ble_started,
                "admin_mode": "token" if ADMIN_TOKEN else "local-only",
                "registrations": {str(status): int(count) for status, count in counts},
            }
        )

    async def list_nodes(self, request: web.Request) -> web.Response:
        self._require_admin(request)
        status = request.query.get("status", "pending").lower()
        if status not in {*VALID_APPROVAL_STATUSES, "all"}:
            raise web.HTTPBadRequest(
                text=json.dumps({"error": "invalid_status"}),
                content_type="application/json",
            )

        sql = REGISTRATION_SELECT + " WHERE active = TRUE"
        parameters: tuple[Any, ...] = ()
        if status != "all":
            sql += " AND approval_status = %s"
            parameters = (status,)
        sql += " ORDER BY requested_at DESC, registration_id DESC"

        with psycopg.connect(DATABASE_URL) as connection:
            rows = connection.execute(sql, parameters).fetchall()
        return web.json_response({"nodes": [registration_payload(row) for row in rows]})

    async def show_node(self, request: web.Request) -> web.Response:
        self._require_admin(request)
        try:
            device_id = normalize_device_id(request.match_info["device_id"])
        except ValueError as error:
            raise web.HTTPBadRequest(text=str(error)) from error

        with psycopg.connect(DATABASE_URL) as connection:
            row = connection.execute(
                REGISTRATION_SELECT
                + " WHERE device_id = %s AND active = TRUE ORDER BY registration_id DESC LIMIT 1",
                (device_id,),
            ).fetchone()
            if row is None:
                raise web.HTTPNotFound(
                    text=json.dumps({"error": "registration_not_found"}),
                    content_type="application/json",
                )

            audit_rows = connection.execute(
                """
                SELECT
                    audit_id,
                    previous_status,
                    new_status,
                    actor,
                    reason,
                    created_at
                FROM devices.registration_audit
                WHERE registration_id = %s
                ORDER BY created_at ASC, audit_id ASC
                """,
                (row[0],),
            ).fetchall()

        payload = registration_payload(row)
        payload["audit"] = [
            {
                "audit_id": int(audit_id),
                "previous_status": previous_status,
                "new_status": new_status,
                "actor": actor,
                "reason": reason,
                "created_at": created_at.isoformat(),
            }
            for (
                audit_id,
                previous_status,
                new_status,
                actor,
                reason,
                created_at,
            ) in audit_rows
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
        new_status = "approved" if decision == "approve" else "rejected"

        with psycopg.connect(DATABASE_URL) as connection:
            with connection.transaction():
                row = connection.execute(
                    """
                    SELECT
                        registration_id,
                        node_uuid,
                        approval_status,
                        token_issued_at
                    FROM public.node_registrations
                    WHERE device_id = %s
                      AND active = TRUE
                    FOR UPDATE
                    """,
                    (device_id,),
                ).fetchone()
                if row is None:
                    raise web.HTTPNotFound(
                        text=json.dumps({"error": "registration_not_found"}),
                        content_type="application/json",
                    )

                registration_id, node_uuid, previous_status, existing_issued_at = row

                if new_status == "approved":
                    if previous_status == "approved" and existing_issued_at is not None:
                        # Idempotent approve: do not rotate a live device's token.
                        pass
                    else:
                        issued_at = datetime.now(timezone.utc)
                        token = derive_bearer_token(
                            self.token_secret,
                            int(registration_id),
                            device_id,
                            node_uuid,
                            issued_at,
                        )
                        token_hash = hash_bearer_token(token)
                        connection.execute(
                            """
                            UPDATE public.node_registrations
                            SET approval_status = 'approved',
                                token_hash = %s,
                                token_issued_at = %s,
                                token_revoked_at = NULL,
                                decided_at = now(),
                                decided_by = %s,
                                decision_reason = %s
                            WHERE registration_id = %s
                            """,
                            (
                                token_hash,
                                issued_at,
                                actor,
                                reason,
                                registration_id,
                            ),
                        )
                else:
                    connection.execute(
                        """
                        UPDATE public.node_registrations
                        SET approval_status = 'rejected',
                            token_hash = NULL,
                            token_issued_at = NULL,
                            token_revoked_at = NULL,
                            decided_at = now(),
                            decided_by = %s,
                            decision_reason = %s
                        WHERE registration_id = %s
                        """,
                        (actor, reason, registration_id),
                    )

                if previous_status != new_status:
                    add_audit(
                        connection,
                        int(registration_id),
                        device_id,
                        str(previous_status),
                        new_status,
                        actor,
                        reason,
                    )

                updated = connection.execute(
                    REGISTRATION_SELECT + " WHERE registration_id = %s",
                    (registration_id,),
                ).fetchone()

        logger.info(
            "Admin decision device_id=%s %s -> %s actor=%s",
            device_id,
            previous_status,
            new_status,
            actor,
        )
        return web.json_response(registration_payload(updated))

    async def start_admin_api(self) -> None:
        app = web.Application()
        app.router.add_get("/health", self.health)
        app.router.add_get("/v1/admin/nodes", self.list_nodes)
        app.router.add_get("/v1/admin/nodes/{device_id}", self.show_node)
        app.router.add_post(
            "/v1/admin/nodes/{device_id}/{decision:approve|reject}",
            self.decide_node,
        )

        self.admin_runner = web.AppRunner(app)
        await self.admin_runner.setup()
        site = web.TCPSite(self.admin_runner, ADMIN_HOST, ADMIN_PORT)
        await site.start()
        logger.info(
            "Admin API listening on %s:%d; admin_mode=%s",
            ADMIN_HOST,
            ADMIN_PORT,
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
                "BLE registration server started as %s; server_uuid=%s; "
                "request_bytes=%d response_bytes=%d credential_uuid=%s",
                SERVER_NAME,
                self.server_uuid,
                REQUEST_SIZE,
                RESPONSE_SIZE,
                CREDENTIAL_UUID,
            )
            await self.stop_event.wait()
        finally:
            if self.ble_started:
                logger.info("Stopping BLE registration server")
                await self.ble.stop()
            if self.admin_runner is not None:
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
        logger.error(
            "BlueZ error: type=%s message=%s",
            getattr(error, "type", "unknown"),
            getattr(error, "text", str(error)),
        )
        raise SystemExit(1) from error
    except KeyboardInterrupt:
        logger.info("Stopped")
    except Exception:
        logger.exception("Tervanix BLE registration server failed")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
