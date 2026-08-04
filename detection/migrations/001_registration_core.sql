CREATE TABLE IF NOT EXISTS registration_server_identity (
    singleton BOOLEAN PRIMARY KEY DEFAULT TRUE CHECK (singleton),
    server_uuid UUID NOT NULL UNIQUE,
    created_at TIMESTAMPTZ NOT NULL DEFAULT now()
);

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

CREATE UNIQUE INDEX IF NOT EXISTS one_active_uuid_per_device
ON node_registrations (device_id)
WHERE active = TRUE;
