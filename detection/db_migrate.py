from __future__ import annotations

import hashlib
import os
from pathlib import Path

import psycopg
from dotenv import load_dotenv

load_dotenv()

MIGRATIONS_DIR = Path(__file__).with_name("migrations")


def database_url() -> str:
    if value := os.getenv("DATABASE_URL"):
        return value
    required = {name: os.getenv(name) for name in ("POSTGRES_USER", "POSTGRES_PASSWORD", "POSTGRES_DB")}
    missing = [name for name, value in required.items() if not value]
    if missing:
        raise RuntimeError("Set DATABASE_URL or define: " + ", ".join(missing))
    return (
        f"postgresql://{required['POSTGRES_USER']}:{required['POSTGRES_PASSWORD']}"
        f"@{os.getenv('POSTGRES_HOST', 'localhost')}:{os.getenv('POSTGRES_PORT', '5432')}"
        f"/{required['POSTGRES_DB']}"
    )


def migrate() -> None:
    files = sorted(MIGRATIONS_DIR.glob("*.sql"))
    if not files:
        raise RuntimeError(f"No migrations found in {MIGRATIONS_DIR}")

    with psycopg.connect(database_url()) as connection:
        connection.execute(
            """
            CREATE TABLE IF NOT EXISTS registration_schema_migrations (
                version TEXT PRIMARY KEY,
                checksum TEXT NOT NULL,
                applied_at TIMESTAMPTZ NOT NULL DEFAULT now()
            )
            """
        )

        applied = dict(
            connection.execute(
                "SELECT version, checksum FROM registration_schema_migrations"
            ).fetchall()
        )

        for path in files:
            sql = path.read_text(encoding="utf-8")
            checksum = hashlib.sha256(sql.encode("utf-8")).hexdigest()
            old_checksum = applied.get(path.name)

            if old_checksum is not None:
                if old_checksum != checksum:
                    raise RuntimeError(
                        f"Applied migration {path.name} was modified; create a new migration instead"
                    )
                continue

            print(f"Applying {path.name}")
            with connection.transaction():
                connection.execute(sql)
                connection.execute(
                    "INSERT INTO registration_schema_migrations(version, checksum) VALUES (%s, %s)",
                    (path.name, checksum),
                )

    print("Registration database is up to date")


if __name__ == "__main__":
    migrate()
