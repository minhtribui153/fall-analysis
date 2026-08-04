from __future__ import annotations

import asyncio
import hmac
import ipaddress
import json
import logging
import os
import signal
import struct
import uuid
from dataclasses import dataclass
from datetime import date, datetime
from typing import Any, Awaitable, Callable

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

PROTOCOL_VERSION = 2
FLAG_HAS_UUID = 1 << 0
FLAG_HAS_SERVER_UUID = 1 << 1

STATUS_ERROR = 0
STATUS_EXISTING_ACCEPTED = 1
STATUS_NEW_ASSIGNED = 2
STATUS_IDENTITY_REJECTED = 3
STATUS_SERVER_REJECTED = 4
STATUS_PENDING_APPROVAL = 5
STATUS_ADMIN_REJECTED = 6

REQUEST_FORMAT = "<BB6s16s16sI"
RESPONSE_FORMAT = "<B16s16sI"
REQUEST_SIZE = struct.calcsize(REQUEST_FORMAT)
RESPONSE_SIZE = struct.calcsize(RESPONSE_FORMAT)
ZERO_UUID = uuid.UUID(int=0)

HTTP_HOST = os.getenv("REGISTRATION_HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("REGISTRATION_HTTP_PORT", "8765"))
ADMIN_TOKEN = os.getenv("REGISTRATION_ADMIN_TOKEN", "").strip()

ADMIN_STATUSES = frozenset({"pending", "approved", "rejected", "all"})
DECISION_ACTIONS = {
    "approve": "approved",
    "reject": "rejected",
}

Handler = Callable[[web.Request], Awaitable[web.StreamResponse]]


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
        raise RuntimeError(
            "Set DATABASE_URL or define: " + ", ".join(missing)
        )

    return f"postgresql://{user}:{password}@{host}:{port}/{database}"


DATABASE_URL = make_database_url()


@dataclass(frozen=True)
class RegistrationRequest:
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
        raise ValueError("hardware identifier must contain exactly 6 bytes")
    return ":".join(f"{byte:02X}" for byte in raw)


def normalize_device_id(value: str) -> str:
    parts = value.strip().upper().split(":")
    if len(parts) != 6 or any(len(part) != 2 for part in parts):
        raise ValueError("device_id must use AA:BB:CC:DD:EE:FF format")

    try:
        bytes(int(part, 16) for part in parts)
    except ValueError as error:
        raise ValueError(
            "device_id must use hexadecimal AA:BB:CC:DD:EE:FF format"
        ) from error

    return ":".join(parts)


def optional_uuid(value: Any) -> uuid.UUID | None:
    if value in (None, "", str(ZERO_UUID)):
        return None
    parsed = uuid.UUID(str(value))
    return None if parsed.int == 0 else parsed


def parse_request(value: bytes) -> RegistrationRequest:
    if len(value) != REQUEST_SIZE:
        raise ValueError(
            f"Expected {REQUEST_SIZE} bytes, received {len(value)}"
        )

    (
        version,
        flags,
        raw_device_id,
        current_uuid_raw,
        server_uuid_raw,
        nonce,
    ) = struct.unpack(REQUEST_FORMAT, value)

    if version != PROTOCOL_VERSION:
        raise ValueError(f"Unsupported protocol version {version}")

    current_uuid = (
        uuid.UUID(bytes=current_uuid_raw)
        if flags & FLAG_HAS_UUID
        else None
    )
    expected_server_uuid = (
        uuid.UUID(bytes=server_uuid_raw)
        if flags & FLAG_HAS_SERVER_UUID
        else None
    )

    return RegistrationRequest(
        device_id=format_device_id(raw_device_id),
        current_uuid=(
            None
            if current_uuid is not None and current_uuid.int == 0
            else current_uuid
        ),
        expected_server_uuid=(
            None
            if expected_server_uuid is not None
            and expected_server_uuid.int == 0
            else expected_server_uuid
        ),
        nonce=nonce,
    )


def build_response(result: RegistrationResult, nonce: int) -> bytes:
    return struct.pack(
        RESPONSE_FORMAT,
        result.status,
        result.node_uuid.bytes,
        result.server_uuid.bytes,
        nonce,
    )


def json_value(value: Any) -> Any:
    if isinstance(value, uuid.UUID):
        return str(value)
    if isinstance(value, (datetime, date)):
        return value.isoformat()
    return value


def row_to_dict(columns: tuple[str, ...], row: tuple[Any, ...]) -> dict[str, Any]:
    return {
        column: json_value(value)
        for column, value in zip(columns, row, strict=True)
    }


def initialize_database() -> uuid.UUID:
    """Load the persistent registrar UUID.

    Database tables are expected to be created by versioned SQL migrations.
    This function deliberately does not create or alter application tables.
    """
    with psycopg.connect(DATABASE_URL) as connection:
        row = connection.execute(
            """
            SELECT server_uuid
            FROM registration_server_identity
            WHERE singleton = TRUE
            """
        ).fetchone()

        if row is None:
            pending = connection.execute(
                """
                SELECT
                    registration_id,
                    claimed_uuid,
                    approval_status
                FROM node_registrations
                WHERE device_id = %s
                AND active = TRUE
                AND approval_status = 'pending'
                FOR UPDATE
                """,
                (request.device_id,),
            ).fetchone()

            if pending is None:
                connection.execute(
                    """
                    INSERT INTO node_registrations (
                        device_id,
                        node_uuid,
                        claimed_uuid,
                        active,
                        approval_status,
                        request_kind,
                        requested_at,
                        last_seen_at
                    )
                    VALUES (
                        %s,
                        %s,
                        %s,
                        TRUE,
                        'pending',
                        'identity_recovery',
                        now(),
                        now()
                    )
                    """,
                    (
                        request.device_id,
                        uuid.uuid4(),
                        request.current_uuid,
                    ),
                )
            else:
                connection.execute(
                    """
                    UPDATE node_registrations
                    SET claimed_uuid = %s,
                        request_kind = 'identity_recovery',
                        requested_at = now(),
                        last_seen_at = now(),
                        revision = revision + 1
                    WHERE registration_id = %s
                    """,
                    (
                        request.current_uuid,
                        pending[0],
                    ),
                )

            return RegistrationResult(
                status=STATUS_PENDING_APPROVAL,
                node_uuid=ZERO_UUID,
                server_uuid=server_uuid,
                reason="identity_recovery_pending",
            )
        else:
            server_uuid = row[0]

    logger.info("Database ready; server_uuid=%s", server_uuid)
    return server_uuid


def register_or_verify(
    request: RegistrationRequest,
    server_uuid: uuid.UUID,
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
            row = connection.execute(
                """
                SELECT
                    registration_id,
                    node_uuid,
                    approval_status
                FROM node_registrations
                WHERE device_id = %s
                  AND active = TRUE
                FOR UPDATE
                """,
                (request.device_id,),
            ).fetchone()

            if request.current_uuid is not None:
                if row is None or row[1] != request.current_uuid:
                    return RegistrationResult(
                        STATUS_IDENTITY_REJECTED,
                        ZERO_UUID,
                        server_uuid,
                        "stored_uuid_not_registered_for_mac",
                    )

                registration_id, node_uuid, approval_status = row
                connection.execute(
                    """
                    UPDATE node_registrations
                    SET last_seen_at = now(),
                        revision = revision + 1
                    WHERE registration_id = %s
                    """,
                    (registration_id,),
                )

                if approval_status == "pending":
                    return RegistrationResult(
                        STATUS_PENDING_APPROVAL,
                        ZERO_UUID,
                        server_uuid,
                        "pending_admin_approval",
                    )
                if approval_status == "rejected":
                    return RegistrationResult(
                        STATUS_ADMIN_REJECTED,
                        ZERO_UUID,
                        server_uuid,
                        "rejected_by_admin",
                    )

                return RegistrationResult(
                    STATUS_EXISTING_ACCEPTED,
                    node_uuid,
                    server_uuid,
                    "verified",
                )

            if row is None:
                assigned_uuid = uuid.uuid4()
                connection.execute(
                    """
                    INSERT INTO node_registrations(
                        device_id,
                        node_uuid,
                        active,
                        approval_status,
                        requested_at,
                        last_seen_at
                    )
                    VALUES (%s, %s, TRUE, 'pending', now(), now())
                    """,
                    (request.device_id, assigned_uuid),
                )
                return RegistrationResult(
                    STATUS_PENDING_APPROVAL,
                    ZERO_UUID,
                    server_uuid,
                    "pending_admin_approval",
                )

            registration_id, assigned_uuid, approval_status = row
            connection.execute(
                """
                UPDATE node_registrations
                SET requested_at = now(),
                    last_seen_at = now(),
                    revision = revision + 1
                WHERE registration_id = %s
                """,
                (registration_id,),
            )

            if approval_status == "pending":
                return RegistrationResult(
                    STATUS_PENDING_APPROVAL,
                    ZERO_UUID,
                    server_uuid,
                    "pending_admin_approval",
                )
            if approval_status == "rejected":
                return RegistrationResult(
                    STATUS_ADMIN_REJECTED,
                    ZERO_UUID,
                    server_uuid,
                    "rejected_by_admin",
                )

            return RegistrationResult(
                STATUS_NEW_ASSIGNED,
                assigned_uuid,
                server_uuid,
                "approved_identity_assigned",
            )


def registered_nodes() -> list[dict[str, str]]:
    with psycopg.connect(DATABASE_URL) as connection:
        rows = connection.execute(
            """
            SELECT device_id, node_uuid
            FROM node_registrations
            WHERE active = TRUE
              AND approval_status = 'approved'
            ORDER BY device_id
            """
        ).fetchall()

    return [
        {"device_id": row[0], "node_uuid": str(row[1])}
        for row in rows
    ]


def list_admin_nodes(status: str) -> list[dict[str, Any]]:
    if status not in ADMIN_STATUSES:
        raise ValueError("invalid registration status")

    status_clause = "" if status == "all" else "AND approval_status = %s"
    parameters: tuple[Any, ...] = () if status == "all" else (status,)

    columns = (
        "registration_id",
        "device_id",
        "node_uuid",
        "approval_status",
        "active",
        "requested_at",
        "registered_at",
        "last_seen_at",
        "decided_at",
        "decided_by",
        "decision_reason",
        "revision",
    )

    with psycopg.connect(DATABASE_URL) as connection:
        rows = connection.execute(
            f"""
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
                revision
            FROM node_registrations
            WHERE active = TRUE
            {status_clause}
            ORDER BY requested_at DESC, registration_id DESC
            """,
            parameters,
        ).fetchall()

    return [row_to_dict(columns, row) for row in rows]


def decide_node(
    device_id: str,
    new_status: str,
    actor: str,
    reason: str | None,
) -> dict[str, Any]:
    if new_status not in {"approved", "rejected"}:
        raise ValueError("decision must be approved or rejected")

    with psycopg.connect(DATABASE_URL) as connection:
        with connection.transaction():
            row = connection.execute(
                """
                SELECT
                    registration_id,
                    node_uuid,
                    approval_status
                FROM node_registrations
                WHERE device_id = %s
                  AND active = TRUE
                FOR UPDATE
                """,
                (device_id,),
            ).fetchone()

            if row is None:
                raise LookupError("active registration not found")

            registration_id, node_uuid, previous_status = row

            if previous_status == new_status:
                return {
                    "device_id": device_id,
                    "node_uuid": str(node_uuid),
                    "previous_status": previous_status,
                    "approval_status": new_status,
                    "actor": actor,
                    "reason": reason or None,
                    "changed": False,
                }

            connection.execute(
                """
                UPDATE node_registrations
                SET approval_status = %s,
                    decided_at = now(),
                    decided_by = %s,
                    decision_reason = %s,
                    revision = revision + 1
                WHERE registration_id = %s
                """,
                (
                    new_status,
                    actor,
                    reason or None,
                    registration_id,
                ),
            )

            connection.execute(
                """
                INSERT INTO registration_decision_audit(
                    registration_id,
                    device_id,
                    node_uuid,
                    previous_status,
                    new_status,
                    actor,
                    reason
                )
                VALUES (%s, %s, %s, %s, %s, %s, %s)
                """,
                (
                    registration_id,
                    device_id,
                    node_uuid,
                    previous_status,
                    new_status,
                    actor,
                    reason or None,
                ),
            )

    return {
        "device_id": device_id,
        "node_uuid": str(node_uuid),
        "previous_status": previous_status,
        "approval_status": new_status,
        "actor": actor,
        "reason": reason or None,
        "changed": True,
    }


def node_details(device_id: str) -> dict[str, Any] | None:
    columns = (
        "registration_id",
        "device_id",
        "node_uuid",
        "approval_status",
        "active",
        "requested_at",
        "registered_at",
        "last_seen_at",
        "decided_at",
        "decided_by",
        "decision_reason",
        "revision",
    )

    with psycopg.connect(DATABASE_URL) as connection:
        row = connection.execute(
            """
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
                revision
            FROM node_registrations
            WHERE device_id = %s
              AND active = TRUE
            """,
            (device_id,),
        ).fetchone()

        if row is None:
            return None

        details = row_to_dict(columns, row)
        audit_rows = connection.execute(
            """
            SELECT
                previous_status,
                new_status,
                actor,
                reason,
                decided_at
            FROM registration_decision_audit
            WHERE registration_id = %s
            ORDER BY decided_at DESC
            """,
            (details["registration_id"],),
        ).fetchall()

    details["audit"] = [
        {
            "previous_status": audit[0],
            "new_status": audit[1],
            "actor": audit[2],
            "reason": audit[3],
            "decided_at": audit[4].isoformat(),
        }
        for audit in audit_rows
    ]
    return details


def peer_ip(request: web.Request) -> ipaddress.IPv4Address | ipaddress.IPv6Address | None:
    """Return the direct TCP peer address.

    Forwarded headers are intentionally ignored. Local-only mode must not be
    bypassable by sending a forged X-Forwarded-For header.
    """
    transport = request.transport
    if transport is None:
        return None

    peer = transport.get_extra_info("peername")
    if peer is None:
        return None

    raw_host = peer[0] if isinstance(peer, tuple) else str(peer)
    try:
        address = ipaddress.ip_address(raw_host)
    except ValueError:
        return None

    if isinstance(address, ipaddress.IPv6Address) and address.ipv4_mapped:
        return address.ipv4_mapped
    return address


def is_local_request(request: web.Request) -> bool:
    address = peer_ip(request)
    return address is not None and address.is_loopback


@web.middleware
async def admin_auth(
    request: web.Request,
    handler: Handler,
) -> web.StreamResponse:
    if not request.path.startswith("/v1/admin/"):
        return await handler(request)

    # A configured token enables authenticated access from any interface on
    # which the HTTP server is listening.
    if ADMIN_TOKEN:
        supplied = request.headers.get("Authorization", "")
        expected = f"Bearer {ADMIN_TOKEN}"
        if not hmac.compare_digest(supplied, expected):
            return web.json_response(
                {
                    "error": "unauthorized",
                    "detail": "A valid bearer token is required",
                },
                status=401,
            )
        return await handler(request)

    # A blank token does not disable the API. It places it in local-only mode.
    if not is_local_request(request):
        address = peer_ip(request)
        logger.warning(
            "Denied tokenless admin request path=%s peer=%s",
            request.path,
            address,
        )
        return web.json_response(
            {
                "error": "local_admin_only",
                "detail": (
                    "REGISTRATION_ADMIN_TOKEN is blank; admin requests "
                    "are accepted only from 127.0.0.1 or ::1"
                ),
            },
            status=403,
        )

    return await handler(request)


class TervanixServer:
    def __init__(self) -> None:
        self.server_uuid = initialize_database()
        self.ble: BlessServer | None = None
        self.characteristic: BlessGATTCharacteristic | None = None
        self.stop_event = asyncio.Event()
        self.http_runner: web.AppRunner | None = None
        self.ble_started = False

    @staticmethod
    def _central_id(request: Any | None) -> str | None:
        if request is None:
            return None
        try:
            return request.get("central_id")
        except (AttributeError, TypeError):
            return getattr(request, "central_id", None)

    def read_request(
        self,
        characteristic: BlessGATTCharacteristic,
        request: Any | None = None,
        **kwargs: Any,
    ) -> bytearray:
        del kwargs
        logger.info(
            "BLE registration response read central=%s bytes=%d",
            self._central_id(request),
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
        registration: RegistrationRequest | None = None

        if str(characteristic.uuid).lower() != REGISTRATION_UUID.lower():
            logger.warning(
                "Ignoring BLE write central=%s characteristic=%s",
                central_id,
                characteristic.uuid,
            )
            return

        try:
            registration = parse_request(bytes(value))
            logger.info(
                "BLE registration request central=%s device_id=%s "
                "current_uuid=%s expected_server_uuid=%s nonce=%u",
                central_id,
                registration.device_id,
                registration.current_uuid,
                registration.expected_server_uuid,
                registration.nonce,
            )

            result = register_or_verify(registration, self.server_uuid)
            characteristic.value = bytearray(
                build_response(result, registration.nonce)
            )

            logger.info(
                "BLE registration response central=%s status=%d reason=%s "
                "node_uuid=%s server_uuid=%s nonce=%u",
                central_id,
                result.status,
                result.reason,
                result.node_uuid,
                result.server_uuid,
                registration.nonce,
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
                build_response(result, registration.nonce if registration else 0)
            )

    async def http_register(self, request: web.Request) -> web.Response:
        try:
            body = await request.json()
            registration = RegistrationRequest(
                device_id=normalize_device_id(body["device_id"]),
                current_uuid=optional_uuid(body.get("current_uuid")),
                expected_server_uuid=optional_uuid(body.get("server_uuid")),
                nonce=int(body.get("nonce", 0)),
            )
            result = await asyncio.to_thread(
                register_or_verify,
                registration,
                self.server_uuid,
            )
        except (
            KeyError,
            ValueError,
            TypeError,
            json.JSONDecodeError,
        ) as error:
            return web.json_response(
                {
                    "status": STATUS_ERROR,
                    "reason": str(error),
                    "server_uuid": str(self.server_uuid),
                },
                status=400,
            )

        payload = {
            "protocol_version": PROTOCOL_VERSION,
            "status": result.status,
            "reason": result.reason,
            "node_uuid": (
                None if result.node_uuid.int == 0 else str(result.node_uuid)
            ),
            "server_uuid": str(result.server_uuid),
            "nonce": registration.nonce,
        }

        if result.status in (STATUS_EXISTING_ACCEPTED, STATUS_NEW_ASSIGNED):
            http_status = 200
        elif result.status == STATUS_PENDING_APPROVAL:
            http_status = 202
        else:
            http_status = 403

        return web.json_response(payload, status=http_status)

    async def http_registry(self, request: web.Request) -> web.Response:
        try:
            authentication = RegistrationRequest(
                device_id=normalize_device_id(
                    request.headers.get("X-Tervanix-Device-ID", "")
                ),
                current_uuid=optional_uuid(
                    request.headers.get("X-Tervanix-Node-UUID")
                ),
                expected_server_uuid=optional_uuid(
                    request.headers.get("X-Tervanix-Server-UUID")
                ),
                nonce=0,
            )
            result = await asyncio.to_thread(
                register_or_verify,
                authentication,
                self.server_uuid,
            )
        except (ValueError, TypeError) as error:
            return web.json_response(
                {"status": STATUS_ERROR, "reason": str(error)},
                status=400,
            )

        if result.status != STATUS_EXISTING_ACCEPTED:
            return web.json_response(
                {"status": result.status, "reason": result.reason},
                status=403,
            )

        nodes = await asyncio.to_thread(registered_nodes)
        return web.json_response(
            {
                "protocol_version": PROTOCOL_VERSION,
                "server_uuid": str(self.server_uuid),
                "registry_version": len(nodes),
                "nodes": nodes,
            }
        )

    async def admin_list(self, request: web.Request) -> web.Response:
        status = request.query.get("status", "pending")
        if status not in ADMIN_STATUSES:
            return web.json_response(
                {
                    "error": "invalid_status",
                    "allowed": sorted(ADMIN_STATUSES),
                },
                status=400,
            )

        nodes = await asyncio.to_thread(list_admin_nodes, status)
        return web.json_response(
            {
                "status_filter": status,
                "count": len(nodes),
                "nodes": nodes,
            }
        )

    async def admin_show(self, request: web.Request) -> web.Response:
        try:
            device_id = normalize_device_id(request.match_info["device_id"])
        except ValueError as error:
            return web.json_response(
                {"error": "invalid_device_id", "detail": str(error)},
                status=400,
            )

        result = await asyncio.to_thread(node_details, device_id)
        if result is None:
            return web.json_response({"error": "not_found"}, status=404)
        return web.json_response(result)

    async def admin_decide(self, request: web.Request) -> web.Response:
        try:
            device_id = normalize_device_id(request.match_info["device_id"])
        except ValueError as error:
            return web.json_response(
                {"error": "invalid_device_id", "detail": str(error)},
                status=400,
            )

        action = request.match_info["action"]
        new_status = DECISION_ACTIONS.get(action)
        if new_status is None:
            return web.json_response(
                {
                    "error": "invalid_action",
                    "allowed": sorted(DECISION_ACTIONS),
                },
                status=400,
            )

        try:
            body = await request.json()
        except json.JSONDecodeError:
            body = {}

        actor = str(body.get("actor", "local-admin")).strip() or "local-admin"
        reason = str(body.get("reason", "")).strip()

        try:
            result = await asyncio.to_thread(
                decide_node,
                device_id,
                new_status,
                actor,
                reason,
            )
        except LookupError:
            return web.json_response({"error": "not_found"}, status=404)

        logger.info(
            "Admin decision action=%s device=%s actor=%s reason=%s peer=%s",
            action,
            device_id,
            actor,
            reason,
            peer_ip(request),
        )
        return web.json_response(result)

    async def health(self, request: web.Request) -> web.Response:
        del request
        return web.json_response(
            {
                "ok": True,
                "server_uuid": str(self.server_uuid),
                "protocol_version": PROTOCOL_VERSION,
                "admin_mode": "token" if ADMIN_TOKEN else "local-only",
            }
        )

    async def configure_ble(self) -> None:
        self.ble = BlessServer(
            name=SERVER_NAME,
            loop=asyncio.get_running_loop(),
        )
        self.ble.read_request_func = self.read_request
        self.ble.write_request_func = self.write_request

        await self.ble.add_new_service(SERVICE_UUID)
        await self.ble.add_new_characteristic(
            SERVICE_UUID,
            REGISTRATION_UUID,
            (
                GATTCharacteristicProperties.read
                | GATTCharacteristicProperties.write
            ),
            bytearray(RESPONSE_SIZE),
            (
                GATTAttributePermissions.readable
                | GATTAttributePermissions.writable
            ),
        )

        self.characteristic = self.ble.get_characteristic(REGISTRATION_UUID)
        if self.characteristic is None:
            raise RuntimeError("Registration characteristic was not created")

    async def start_http(self) -> None:
        application = web.Application(
            client_max_size=16 * 1024,
            middlewares=[admin_auth],
        )
        application.router.add_post("/v1/register", self.http_register)
        application.router.add_get("/v1/registry", self.http_registry)
        application.router.add_get("/v1/admin/nodes", self.admin_list)
        application.router.add_get(
            "/v1/admin/nodes/{device_id}",
            self.admin_show,
        )
        application.router.add_post(
            "/v1/admin/nodes/{device_id}/{action}",
            self.admin_decide,
        )
        application.router.add_get("/health", self.health)

        self.http_runner = web.AppRunner(application)
        await self.http_runner.setup()
        site = web.TCPSite(self.http_runner, HTTP_HOST, HTTP_PORT)
        await site.start()

        logger.info(
            "Wi-Fi registration API listening on %s:%d; admin_mode=%s",
            HTTP_HOST,
            HTTP_PORT,
            "token" if ADMIN_TOKEN else "local-only",
        )

    async def run(self) -> None:
        await self.configure_ble()
        await self.start_http()

        if self.ble is None:
            raise RuntimeError("BLE server is not configured")

        try:
            await self.ble.start()
            self.ble_started = True
            logger.info("BLE registration server started as %s", SERVER_NAME)
            await self.stop_event.wait()
        finally:
            if self.ble_started:
                logger.info("Stopping BLE registration server")
                await self.ble.stop()
            if self.http_runner is not None:
                logger.info("Stopping HTTP registration server")
                await self.http_runner.cleanup()

    def stop(self) -> None:
        self.stop_event.set()


async def async_main() -> None:
    application = TervanixServer()
    loop = asyncio.get_running_loop()

    for received_signal in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(received_signal, application.stop)
        except NotImplementedError:
            # add_signal_handler is not available on every event loop/platform.
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
        logger.exception("Tervanix registration server failed")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
