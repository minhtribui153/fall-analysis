from __future__ import annotations

import asyncio
import logging
import os
import signal
import struct
import uuid
from dataclasses import dataclass
from typing import Any

import psycopg
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
    level=logging.INFO,
    format="%(asctime)s %(levelname)s %(name)s: %(message)s",
)
logger = logging.getLogger("tervanix.server")


SERVER_NAME = "TVX-REG"

SERVICE_UUID = "7db20000-6e8d-4d8d-a991-2cf812340000"
REGISTRATION_UUID = "7db20001-6e8d-4d8d-a991-2cf812340000"

PROTOCOL_VERSION = 1
FLAG_HAS_UUID = 1 << 0

STATUS_ERROR = 0
STATUS_EXISTING_ACCEPTED = 1
STATUS_NEW_ASSIGNED = 2

REQUEST_FORMAT = "<BB6s16sI"
RESPONSE_FORMAT = "<B16sI"

REQUEST_SIZE = struct.calcsize(REQUEST_FORMAT)
RESPONSE_SIZE = struct.calcsize(RESPONSE_FORMAT)

ZERO_UUID = uuid.UUID(int=0)


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

    return (
        f"postgresql://{user}:{password}"
        f"@{host}:{port}/{database}"
    )


DATABASE_URL = make_database_url()


# NOTE ON NAMING: the wire protocol (registration_protocol.hpp /
# REQUEST_FORMAT below) still calls the 6 raw MAC bytes coming off the
# device "hardware_id" -- that's fine, it's just describing what's on the
# wire. But everywhere this touches Postgres, the column/variable is
# "device_id" (a formatted "AA:BB:CC:DD:EE:FF" string), matching
# devices.identification.device_id used by the Kafka -> Postgres side of
# the pipeline (detection/kafka_to_postgres.py). Previously this table
# used a separate "hardware_id BYTEA" column, which meant the two halves
# of the pipeline had no shared key to join on.
SCHEMA_SQL = """
CREATE TABLE IF NOT EXISTS node_registrations (
    registration_id BIGSERIAL PRIMARY KEY,

    device_id TEXT NOT NULL
        CHECK (device_id ~ '^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$'),

    node_uuid UUID NOT NULL UNIQUE,

    active BOOLEAN NOT NULL DEFAULT TRUE,

    registered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    delinked_at TIMESTAMPTZ
);

CREATE UNIQUE INDEX IF NOT EXISTS
    one_active_uuid_per_device
ON node_registrations (device_id)
WHERE active = TRUE;
"""


@dataclass(frozen=True)
class RegistrationRequest:
    device_id: str
    current_uuid: uuid.UUID | None
    nonce: int


@dataclass(frozen=True)
class RegistrationResult:
    status: int
    node_uuid: uuid.UUID


def format_device_id(raw_hardware_id: bytes) -> str:
    """Formats the 6 raw MAC bytes off the wire into the same
    "AA:BB:CC:DD:EE:FF" text form used everywhere else as device_id
    (devices.identification.device_id, MQTT payloads' device_id field,
    node_registrations.device_id)."""
    return ":".join(f"{byte:02X}" for byte in raw_hardware_id)


def parse_request(value: bytes) -> RegistrationRequest:
    if len(value) != REQUEST_SIZE:
        raise ValueError(
            f"Expected {REQUEST_SIZE} bytes, received {len(value)}"
        )

    (
        version,
        flags,
        raw_hardware_id,
        current_uuid_bytes,
        nonce,
    ) = struct.unpack(REQUEST_FORMAT, value)

    if version != PROTOCOL_VERSION:
        raise ValueError(f"Unsupported protocol version {version}")

    current_uuid: uuid.UUID | None = None

    if flags & FLAG_HAS_UUID:
        parsed = uuid.UUID(bytes=current_uuid_bytes)
        if parsed.int != 0:
            current_uuid = parsed

    return RegistrationRequest(
        device_id=format_device_id(raw_hardware_id),
        current_uuid=current_uuid,
        nonce=nonce,
    )


def build_response(
    status: int,
    node_uuid: uuid.UUID,
    nonce: int,
) -> bytes:
    return struct.pack(
        RESPONSE_FORMAT,
        status,
        node_uuid.bytes,
        nonce,
    )


def initialize_database() -> None:
    with psycopg.connect(DATABASE_URL) as connection:
        connection.execute(SCHEMA_SQL)

    logger.info("PostgreSQL schema ready")


def assign_uuid(
    request: RegistrationRequest,
) -> RegistrationResult:
    with psycopg.connect(DATABASE_URL) as connection:
        if request.current_uuid is not None:
            row = connection.execute(
                """
                SELECT node_uuid
                FROM node_registrations
                WHERE device_id = %s
                  AND node_uuid = %s
                  AND active = TRUE
                FOR UPDATE
                """,
                (
                    request.device_id,
                    request.current_uuid,
                ),
            ).fetchone()

            if row is not None:
                connection.execute(
                    """
                    UPDATE node_registrations
                    SET last_seen_at = now()
                    WHERE device_id = %s
                      AND node_uuid = %s
                      AND active = TRUE
                    """,
                    (
                        request.device_id,
                        request.current_uuid,
                    ),
                )

                return RegistrationResult(
                    status=STATUS_EXISTING_ACCEPTED,
                    node_uuid=request.current_uuid,
                )

        connection.execute(
            """
            UPDATE node_registrations
            SET active = FALSE,
                delinked_at = COALESCE(delinked_at, now())
            WHERE device_id = %s
              AND active = TRUE
            """,
            (request.device_id,),
        )

        assigned = uuid.uuid4()

        connection.execute(
            """
            INSERT INTO node_registrations (
                device_id,
                node_uuid,
                active
            )
            VALUES (%s, %s, TRUE)
            """,
            (
                request.device_id,
                assigned,
            ),
        )

        return RegistrationResult(
            status=STATUS_NEW_ASSIGNED,
            node_uuid=assigned,
        )


class TervanixServer:
    def __init__(self) -> None:
        self.server: BlessServer | None = None
        self.registration_characteristic: (
            BlessGATTCharacteristic | None
        ) = None
        self.stop_event = asyncio.Event()
        self.started = False

    def read_request(
        self,
        characteristic: BlessGATTCharacteristic,
        **kwargs: Any,
    ) -> bytearray:
        del kwargs

        logger.info(
            "Registration value read (%d bytes)",
            len(characteristic.value),
        )
        return characteristic.value

    def write_request(
        self,
        characteristic: BlessGATTCharacteristic,
        value: bytes | bytearray,
        **kwargs: Any,
    ) -> None:
        del kwargs

        if str(characteristic.uuid).lower() != REGISTRATION_UUID:
            logger.warning(
                "Ignoring write to %s",
                characteristic.uuid,
            )
            return

        try:
            request = parse_request(bytes(value))

            logger.info(
                "Request device_id=%s current_uuid=%s nonce=%u",
                request.device_id,
                request.current_uuid,
                request.nonce,
            )

            result = assign_uuid(request)

            characteristic.value = bytearray(
                build_response(
                    result.status,
                    result.node_uuid,
                    request.nonce,
                )
            )

            logger.info(
                "Response status=%d uuid=%s nonce=%u",
                result.status,
                result.node_uuid,
                request.nonce,
            )

        except Exception:
            logger.exception("Registration failed")

            characteristic.value = bytearray(
                build_response(
                    STATUS_ERROR,
                    ZERO_UUID,
                    0,
                )
            )

    async def configure(self) -> None:
        loop = asyncio.get_running_loop()

        self.server = BlessServer(
            name=SERVER_NAME,
            loop=loop,
        )

        self.server.read_request_func = self.read_request
        self.server.write_request_func = self.write_request

        await self.server.add_new_service(SERVICE_UUID)

        properties = (
            GATTCharacteristicProperties.read
            | GATTCharacteristicProperties.write
        )
        permissions = (
            GATTAttributePermissions.readable
            | GATTAttributePermissions.writeable
        )

        await self.server.add_new_characteristic(
            SERVICE_UUID,
            REGISTRATION_UUID,
            properties,
            bytearray(RESPONSE_SIZE),
            permissions,
        )

        self.registration_characteristic = (
            self.server.get_characteristic(REGISTRATION_UUID)
        )

        if self.registration_characteristic is None:
            raise RuntimeError(
                "Registration characteristic was not created"
            )

    async def run(self) -> None:
        initialize_database()
        await self.configure()

        if self.server is None:
            raise RuntimeError("BLE server is not configured")

        try:
            logger.info("Advertising name: %s", SERVER_NAME)
            logger.info("Service UUID: %s", SERVICE_UUID)
            logger.info(
                "Registration characteristic: %s",
                REGISTRATION_UUID,
            )

            await self.server.start()
            self.started = True

            logger.info("BLE registration server started")
            await self.stop_event.wait()

        finally:
            if self.started and self.server is not None:
                logger.info("Stopping BLE server")
                await self.server.stop()

    def stop(self) -> None:
        self.stop_event.set()


async def async_main() -> None:
    application = TervanixServer()
    loop = asyncio.get_running_loop()

    for received_signal in (signal.SIGINT, signal.SIGTERM):
        loop.add_signal_handler(
            received_signal,
            application.stop,
        )

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
        logger.exception("Tervanix server failed")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
