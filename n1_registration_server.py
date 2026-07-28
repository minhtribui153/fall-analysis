from __future__ import annotations

import asyncio
import json
import logging
import os
import signal
import struct
import time
import uuid
from dataclasses import dataclass
from typing import Any

import paho.mqtt.client as mqtt
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

logging.getLogger("bless.backends.winrt.server").setLevel(logging.INFO)
logging.getLogger("bless.backends.characteristic").setLevel(logging.INFO)
logging.getLogger("bless.backends.server").setLevel(logging.INFO)

logger = logging.getLogger("tervanix.gateway")


def _patch_bless_windows_adapter() -> None:
    """Bypass Bless's low-level Windows adapter helper when it cannot
    construct a valid CreateFile device path.

    The gateway does not request Bless to rename the Windows Bluetooth radio.
    The Nesso discovers this server by the advertised service UUID as well as
    by name, so the helper is unnecessary for registration/telemetry.
    """
    if os.name != "nt":
        return

    import bless.backends.winrt.server as bless_winrt_server

    class _NoOpBLEAdapter:
        def set_local_name(self, local_name: str) -> None:
            logger.warning(
                "Skipping Windows Bluetooth adapter rename to %s; "
                "Nesso discovery will use the service UUID",
                local_name,
            )

    bless_winrt_server.BLEAdapter = _NoOpBLEAdapter


# ---------------------------------------------------------------------------
# BLE UUIDs
# ---------------------------------------------------------------------------
SERVER_NAME = "TVX-REG"

SERVICE_UUID = "7db20000-6e8d-4d8d-a991-2cf812340000"
REGISTRATION_UUID = "7db20001-6e8d-4d8d-a991-2cf812340000"
TELEMETRY_UUID = "7db20002-6e8d-4d8d-a991-2cf812340000"


# ---------------------------------------------------------------------------
# Registration protocol
# ---------------------------------------------------------------------------
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


# ---------------------------------------------------------------------------
# Telemetry protocol
# Must match tervanix-n1/include/telemetry_protocol.hpp exactly.
# ---------------------------------------------------------------------------
TELEMETRY_VERSION = 1
TELEMETRY_SAMPLES_PER_PACKET = 2
TELEMETRY_SAMPLE_PERIOD_MS = 10  # 100 Hz
MQTT_BATCH_SIZE = 10             # preserve the old MQTT payload shape/rate

TELEMETRY_HEADER_FORMAT = "<BB6sI"
TELEMETRY_SAMPLE_FORMAT = "<ffffff"

TELEMETRY_HEADER_SIZE = struct.calcsize(TELEMETRY_HEADER_FORMAT)
TELEMETRY_SAMPLE_SIZE = struct.calcsize(TELEMETRY_SAMPLE_FORMAT)
TELEMETRY_PACKET_SIZE = (
    TELEMETRY_HEADER_SIZE
    + TELEMETRY_SAMPLES_PER_PACKET * TELEMETRY_SAMPLE_SIZE
)

assert TELEMETRY_HEADER_SIZE == 12
assert TELEMETRY_SAMPLE_SIZE == 24
assert TELEMETRY_PACKET_SIZE == 60


# ---------------------------------------------------------------------------
# MQTT configuration
# ---------------------------------------------------------------------------
MQTT_HOST = os.getenv("MQTT_HOST", "127.0.0.1")
MQTT_PORT = int(os.getenv("MQTT_PORT", "1883"))
MQTT_TOPIC = os.getenv("MQTT_TOPIC", "sensor-readings")
MQTT_USERNAME = os.getenv("MQTT_USERNAME")
MQTT_PASSWORD = os.getenv("MQTT_PASSWORD")
MQTT_CLIENT_ID = os.getenv("MQTT_CLIENT_ID", "tervanix-ble-gateway")


# ---------------------------------------------------------------------------
# PostgreSQL configuration
# ---------------------------------------------------------------------------
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


@dataclass(frozen=True)
class TelemetryPacket:
    device_id: str
    first_sample_sequence: int
    samples: list[tuple[float, float, float, float, float, float]]


def format_device_id(raw_hardware_id: bytes) -> str:
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


def parse_telemetry(value: bytes) -> TelemetryPacket:
    if len(value) != TELEMETRY_PACKET_SIZE:
        raise ValueError(
            "Expected telemetry packet of "
            f"{TELEMETRY_PACKET_SIZE} bytes, received {len(value)}"
        )

    (
        version,
        sample_count,
        raw_hardware_id,
        first_sample_sequence,
    ) = struct.unpack_from(TELEMETRY_HEADER_FORMAT, value, 0)

    if version != TELEMETRY_VERSION:
        raise ValueError(f"Unsupported telemetry version {version}")

    if not 1 <= sample_count <= TELEMETRY_SAMPLES_PER_PACKET:
        raise ValueError(f"Invalid telemetry sample count {sample_count}")

    samples: list[tuple[float, float, float, float, float, float]] = []

    for index in range(sample_count):
        offset = TELEMETRY_HEADER_SIZE + index * TELEMETRY_SAMPLE_SIZE
        samples.append(
            struct.unpack_from(TELEMETRY_SAMPLE_FORMAT, value, offset)
        )

    return TelemetryPacket(
        device_id=format_device_id(raw_hardware_id),
        first_sample_sequence=first_sample_sequence,
        samples=samples,
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


class MqttBridge:
    def __init__(self) -> None:
        self.client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=MQTT_CLIENT_ID,
            protocol=mqtt.MQTTv311,
        )

        if MQTT_USERNAME:
            self.client.username_pw_set(
                MQTT_USERNAME,
                MQTT_PASSWORD,
            )

        self.client.on_connect = self._on_connect
        self.client.on_disconnect = self._on_disconnect
        self.client.reconnect_delay_set(min_delay=1, max_delay=30)
        self.client.max_queued_messages_set(2000)

        self._last_publish_warning = 0.0

    def _on_connect(
        self,
        client: mqtt.Client,
        userdata: Any,
        connect_flags: mqtt.ConnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del client, userdata, connect_flags, properties

        if reason_code == 0:
            logger.info(
                "MQTT connected to %s:%d; topic=%s",
                MQTT_HOST,
                MQTT_PORT,
                MQTT_TOPIC,
            )
        else:
            logger.error("MQTT connection rejected: %s", reason_code)

    def _on_disconnect(
        self,
        client: mqtt.Client,
        userdata: Any,
        disconnect_flags: mqtt.DisconnectFlags,
        reason_code: mqtt.ReasonCode,
        properties: mqtt.Properties | None,
    ) -> None:
        del client, userdata, disconnect_flags, properties

        if reason_code != 0:
            logger.warning(
                "MQTT disconnected unexpectedly: %s; reconnecting",
                reason_code,
            )

    def start(self) -> None:
        logger.info(
            "Starting MQTT bridge to %s:%d",
            MQTT_HOST,
            MQTT_PORT,
        )
        self.client.connect_async(
            MQTT_HOST,
            MQTT_PORT,
            keepalive=60,
        )
        self.client.loop_start()

    def stop(self) -> None:
        try:
            self.client.disconnect()
        finally:
            self.client.loop_stop()

    def publish(self, readings: list[dict[str, Any]]) -> None:
        payload = json.dumps(
            readings,
            separators=(",", ":"),
        )

        info = self.client.publish(
            MQTT_TOPIC,
            payload,
            qos=1,
            retain=False,
        )

        if info.rc != mqtt.MQTT_ERR_SUCCESS:
            now = time.monotonic()
            if now - self._last_publish_warning >= 5.0:
                logger.warning(
                    "MQTT publish not accepted yet (rc=%s); "
                    "check broker connectivity",
                    info.rc,
                )
                self._last_publish_warning = now


class TervanixGateway:
    def __init__(self) -> None:
        self.server: BlessServer | None = None
        self.registration_characteristic: BlessGATTCharacteristic | None = None
        self.telemetry_characteristic: BlessGATTCharacteristic | None = None

        self.mqtt = MqttBridge()
        self.stop_event = asyncio.Event()
        self.started = False

        # Nesso always registers before telemetry. This cache prevents random
        # BLE writers from injecting readings without a successful registration.
        self.registered_devices: set[str] = set()

        # Keep the old MQTT contract: arrays of 10 readings per message.
        self.pending_readings: dict[str, list[dict[str, Any]]] = {}

        # Expected next sample sequence for basic dropped-packet detection.
        self.expected_sequence: dict[str, int] = {}

    def read_request(
        self,
        characteristic: BlessGATTCharacteristic,
        request: Any = None,
        **kwargs: Any,
    ) -> bytearray:
        # Current Bless passes a request object as the third positional
        # callback argument. Keeping it optional also stays compatible with
        # older Bless versions that called the handler with only the
        # characteristic.
        del request, kwargs
        return characteristic.value

    def _handle_registration(
        self,
        characteristic: BlessGATTCharacteristic,
        value: bytes | bytearray,
    ) -> None:
        try:
            request = parse_request(bytes(value))

            logger.info(
                "Registration request device_id=%s current_uuid=%s nonce=%u",
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

            self.registered_devices.add(request.device_id)
            self.expected_sequence.pop(request.device_id, None)

            logger.info(
                "Registration response status=%d uuid=%s device_id=%s",
                result.status,
                result.node_uuid,
                request.device_id,
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

    def _handle_telemetry(
        self,
        value: bytes | bytearray,
    ) -> None:
        try:
            packet = parse_telemetry(bytes(value))

            if packet.device_id not in self.registered_devices:
                logger.warning(
                    "Dropping telemetry from unregistered device %s",
                    packet.device_id,
                )
                return

            expected = self.expected_sequence.get(packet.device_id)
            if (
                expected is not None
                and packet.first_sample_sequence != expected
            ):
                logger.warning(
                    "Telemetry sequence gap for %s: expected=%u received=%u",
                    packet.device_id,
                    expected,
                    packet.first_sample_sequence,
                )

            self.expected_sequence[packet.device_id] = (
                packet.first_sample_sequence + len(packet.samples)
            ) & 0xFFFFFFFF

            # No Wi-Fi/SNTP exists on the Nesso anymore, so the gateway owns
            # wall-clock timestamps. The newest sample is anchored to receipt
            # time and earlier samples are spaced by the known 10 ms period.
            received_ms = time.time_ns() // 1_000_000
            first_ts = received_ms - (
                (len(packet.samples) - 1)
                * TELEMETRY_SAMPLE_PERIOD_MS
            )

            buffer = self.pending_readings.setdefault(
                packet.device_id,
                [],
            )

            for index, sample in enumerate(packet.samples):
                ax, ay, az, gx, gy, gz = sample

                buffer.append(
                    {
                        "device_id": packet.device_id,
                        "ts": first_ts
                        + index * TELEMETRY_SAMPLE_PERIOD_MS,
                        "ax": ax,
                        "ay": ay,
                        "az": az,
                        "gx": gx,
                        "gy": gy,
                        "gz": gz,
                    }
                )

            while len(buffer) >= MQTT_BATCH_SIZE:
                batch = buffer[:MQTT_BATCH_SIZE]
                del buffer[:MQTT_BATCH_SIZE]
                self.mqtt.publish(batch)

        except Exception:
            logger.exception("Telemetry packet failed")

    def write_request(
        self,
        characteristic: BlessGATTCharacteristic,
        value: bytes | bytearray,
        request: Any = None,
        **kwargs: Any,
    ) -> None:
        # See read_request(): accept both the current Bless callback shape
        # and the older two-argument form.
        del request, kwargs

        characteristic_uuid = str(characteristic.uuid).lower()

        if characteristic_uuid == REGISTRATION_UUID:
            self._handle_registration(characteristic, value)
            return

        if characteristic_uuid == TELEMETRY_UUID:
            self._handle_telemetry(value)
            return

        logger.warning(
            "Ignoring write to unknown characteristic %s",
            characteristic.uuid,
        )

    async def configure(self) -> None:
        loop = asyncio.get_running_loop()

        _patch_bless_windows_adapter()

        self.server = BlessServer(
            name=SERVER_NAME,
            loop=loop,
        )

        self.server.read_request_func = self.read_request
        self.server.write_request_func = self.write_request

        await self.server.add_new_service(SERVICE_UUID)

        registration_properties = (
            GATTCharacteristicProperties.read
            | GATTCharacteristicProperties.write
        )
        registration_permissions = (
            GATTAttributePermissions.readable
            | GATTAttributePermissions.writable
        )

        await self.server.add_new_characteristic(
            SERVICE_UUID,
            REGISTRATION_UUID,
            registration_properties,
            bytearray(RESPONSE_SIZE),
            registration_permissions,
        )

        telemetry_properties = (
            GATTCharacteristicProperties.write
            | GATTCharacteristicProperties.write_without_response
        )
        telemetry_permissions = GATTAttributePermissions.writable

        await self.server.add_new_characteristic(
            SERVICE_UUID,
            TELEMETRY_UUID,
            telemetry_properties,
            bytearray(TELEMETRY_PACKET_SIZE),
            telemetry_permissions,
        )

        self.registration_characteristic = (
            self.server.get_characteristic(REGISTRATION_UUID)
        )
        self.telemetry_characteristic = (
            self.server.get_characteristic(TELEMETRY_UUID)
        )

        if self.registration_characteristic is None:
            raise RuntimeError(
                "Registration characteristic was not created"
            )

        if self.telemetry_characteristic is None:
            raise RuntimeError(
                "Telemetry characteristic was not created"
            )

    def flush_pending_mqtt(self) -> None:
        for device_id, buffer in self.pending_readings.items():
            if buffer:
                logger.info(
                    "Publishing final partial batch for %s (%d readings)",
                    device_id,
                    len(buffer),
                )
                self.mqtt.publish(buffer.copy())
                buffer.clear()

    async def run(self) -> None:
        initialize_database()
        self.mqtt.start()
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
            logger.info(
                "Telemetry characteristic: %s",
                TELEMETRY_UUID,
            )

            await self.server.start()
            self.started = True

            logger.info("BLE registration + telemetry gateway started")
            await self.stop_event.wait()

        finally:
            self.flush_pending_mqtt()

            if self.started and self.server is not None:
                logger.info("Stopping BLE server")
                await self.server.stop()

            self.mqtt.stop()

    def stop(self) -> None:
        self.stop_event.set()


async def async_main() -> None:
    application = TervanixGateway()
    loop = asyncio.get_running_loop()

    # asyncio signal handlers are Unix-only. On Windows, Ctrl+C is handled
    # by the KeyboardInterrupt path in main().
    if os.name != "nt":
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
        logger.exception("Tervanix gateway failed")
        raise SystemExit(1)


if __name__ == "__main__":
    main()
