-- ============================================================================
-- Fall Analysis PostgreSQL schema (device_id version)
--
-- Reconstructed from:
--   1. The project's BLE registration gateway and Kafka-to-PostgreSQL pipeline.
--   2. The supplied Drizzle schema.
--   3. The project's timestamp/view migration history.
--
-- Intended for a NEW / EMPTY fall_analysis database.
-- The canonical physical-device key is device_id, formatted as:
--     AA:BB:CC:DD:EE:FF
-- ============================================================================

BEGIN;

CREATE SCHEMA IF NOT EXISTS devices;
CREATE SCHEMA IF NOT EXISTS workers;
CREATE SCHEMA IF NOT EXISTS monitoring;
CREATE SCHEMA IF NOT EXISTS fall_detection;

-- --------------------------------------------------------------------------
-- Device identity
-- --------------------------------------------------------------------------
CREATE TABLE devices.identification (
    device_id TEXT PRIMARY KEY,
    node_uuid UUID NOT NULL UNIQUE,
    ble_address TEXT,
    registered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now(),

    CONSTRAINT identification_device_id_format_check
        CHECK (device_id ~ '^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$')
);

COMMENT ON TABLE devices.identification IS
    'Canonical device identity used by monitoring sessions and telemetry ingestion.';

COMMENT ON COLUMN devices.identification.device_id IS
    'Stable 6-byte hardware identifier formatted as AA:BB:CC:DD:EE:FF.';

-- --------------------------------------------------------------------------
-- BLE registration history
-- Kept in public because n1_registration_server.py currently addresses it as
-- node_registrations without a schema prefix.
-- --------------------------------------------------------------------------
CREATE TABLE public.node_registrations (
    registration_id BIGSERIAL PRIMARY KEY,
    device_id TEXT NOT NULL,
    node_uuid UUID NOT NULL UNIQUE,
    active BOOLEAN NOT NULL DEFAULT TRUE,
    registered_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    last_seen_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    delinked_at TIMESTAMPTZ,

    CONSTRAINT node_registrations_device_id_format_check
        CHECK (device_id ~ '^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$'),
    CONSTRAINT node_registrations_delink_state_check
        CHECK (
            (active = TRUE AND delinked_at IS NULL)
            OR active = FALSE
        )
);

CREATE UNIQUE INDEX one_active_registration_per_device
    ON public.node_registrations (device_id)
    WHERE active = TRUE;

CREATE INDEX idx_node_registrations_device_history
    ON public.node_registrations (device_id, registered_at DESC);

COMMENT ON TABLE public.node_registrations IS
    'BLE registration history. Only one active registration is allowed per device_id.';

-- --------------------------------------------------------------------------
-- Workers
-- --------------------------------------------------------------------------
CREATE TABLE workers.workers_list (
    worker_id SERIAL PRIMARY KEY,
    worker_code VARCHAR(20) NOT NULL UNIQUE,
    site_assigned VARCHAR(100),
    role VARCHAR(50),
    status VARCHAR(20) NOT NULL DEFAULT 'active',
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),

    CONSTRAINT workers_list_status_check
        CHECK (status IN ('active', 'inactive'))
);

-- --------------------------------------------------------------------------
-- Monitoring sessions
-- --------------------------------------------------------------------------
CREATE TABLE monitoring.sessions (
    session_id SERIAL PRIMARY KEY,
    device_id TEXT NOT NULL,
    worker_id INTEGER,
    start_time TIMESTAMPTZ NOT NULL DEFAULT now(),
    end_time TIMESTAMPTZ,
    sample_rate_hz INTEGER NOT NULL DEFAULT 100,

    CONSTRAINT fk_sessions_device
        FOREIGN KEY (device_id)
        REFERENCES devices.identification (device_id)
        ON DELETE RESTRICT,

    CONSTRAINT fk_sessions_worker
        FOREIGN KEY (worker_id)
        REFERENCES workers.workers_list (worker_id)
        ON DELETE SET NULL,

    CONSTRAINT sessions_sample_rate_check
        CHECK (sample_rate_hz > 0),

    CONSTRAINT sessions_time_order_check
        CHECK (end_time IS NULL OR end_time >= start_time)
);

CREATE INDEX idx_sessions_device
    ON monitoring.sessions (device_id);

CREATE INDEX idx_sessions_open
    ON monitoring.sessions (device_id)
    WHERE end_time IS NULL;

CREATE INDEX idx_sessions_worker
    ON monitoring.sessions (worker_id);

-- --------------------------------------------------------------------------
-- Raw IMU readings
-- --------------------------------------------------------------------------
CREATE TABLE monitoring.readings (
    reading_id BIGSERIAL PRIMARY KEY,
    session_id INTEGER NOT NULL,
    ts TIMESTAMPTZ NOT NULL,
    ax REAL NOT NULL,
    ay REAL NOT NULL,
    az REAL NOT NULL,
    gx REAL NOT NULL DEFAULT 0,
    gy REAL NOT NULL DEFAULT 0,
    gz REAL NOT NULL DEFAULT 0,
    smv REAL,
    gyro_magnitude REAL,

    CONSTRAINT fk_readings_session
        FOREIGN KEY (session_id)
        REFERENCES monitoring.sessions (session_id)
        ON DELETE CASCADE
);

CREATE INDEX idx_readings_session_ts
    ON monitoring.readings (session_id, ts);

-- --------------------------------------------------------------------------
-- Fall / near-miss classification
-- --------------------------------------------------------------------------
CREATE TABLE fall_detection.classification (
    classification_id BIGSERIAL PRIMARY KEY,
    session_id INTEGER NOT NULL,
    ts TIMESTAMPTZ NOT NULL,
    event_type VARCHAR(20) NOT NULL,
    fall_type VARCHAR(10),
    max_smv REAL,
    max_rotation REAL,
    freefall_duration REAL,
    tilt_change REAL,
    responsiveness VARCHAR(10),
    activity_before_event VARCHAR(20),
    created_at TIMESTAMPTZ NOT NULL DEFAULT now(),

    CONSTRAINT fk_classification_session
        FOREIGN KEY (session_id)
        REFERENCES monitoring.sessions (session_id)
        ON DELETE CASCADE,

    CONSTRAINT classification_event_type_check
        CHECK (event_type IN ('NORMAL', 'NEAR_MISS', 'FALL')),

    CONSTRAINT classification_fall_type_check
        CHECK (fall_type IS NULL OR fall_type IN ('FFH', 'STF')),

    CONSTRAINT classification_responsiveness_check
        CHECK (responsiveness IS NULL OR responsiveness IN ('still', 'active'))
);

CREATE INDEX idx_classification_session_ts
    ON fall_detection.classification (session_id, ts);

CREATE INDEX idx_classification_events
    ON fall_detection.classification (event_type)
    WHERE event_type <> 'NORMAL';

-- --------------------------------------------------------------------------
-- Activity changes
-- --------------------------------------------------------------------------
CREATE TABLE fall_detection.activity_log (
    activity_id BIGSERIAL PRIMARY KEY,
    session_id INTEGER NOT NULL,
    ts TIMESTAMPTZ NOT NULL,
    activity_state VARCHAR(20) NOT NULL,

    CONSTRAINT fk_activity_log_session
        FOREIGN KEY (session_id)
        REFERENCES monitoring.sessions (session_id)
        ON DELETE CASCADE,

    CONSTRAINT activity_log_activity_state_check
        CHECK (
            activity_state IN (
                'stationary',
                'walking',
                'running',
                'jumping',
                'unknown'
            )
        )
);

CREATE INDEX idx_activity_session_ts
    ON fall_detection.activity_log (session_id, ts);

-- --------------------------------------------------------------------------
-- Alerts raised for classifications
-- --------------------------------------------------------------------------
CREATE TABLE fall_detection.alerts (
    alert_id SERIAL PRIMARY KEY,
    classification_id BIGINT NOT NULL,
    raised_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    severity VARCHAR(10) NOT NULL DEFAULT 'high',
    acknowledged BOOLEAN NOT NULL DEFAULT FALSE,
    acknowledged_by INTEGER,
    acknowledged_at TIMESTAMPTZ,
    notes TEXT,

    CONSTRAINT fk_alerts_classification
        FOREIGN KEY (classification_id)
        REFERENCES fall_detection.classification (classification_id)
        ON DELETE CASCADE,

    CONSTRAINT fk_alerts_acknowledged_by
        FOREIGN KEY (acknowledged_by)
        REFERENCES workers.workers_list (worker_id)
        ON DELETE SET NULL,

    CONSTRAINT alerts_severity_check
        CHECK (severity IN ('low', 'medium', 'high')),

    CONSTRAINT alerts_acknowledgement_check
        CHECK (
            (acknowledged = FALSE AND acknowledged_at IS NULL)
            OR acknowledged = TRUE
        )
);

CREATE INDEX idx_alerts_unacknowledged
    ON fall_detection.alerts (raised_at)
    WHERE acknowledged = FALSE;

-- --------------------------------------------------------------------------
-- Dashboard view
-- --------------------------------------------------------------------------
CREATE OR REPLACE VIEW fall_detection.v_events_detail AS
SELECT
    c.classification_id,
    c.ts,
    c.event_type,
    c.fall_type,
    c.max_smv,
    c.max_rotation,
    c.responsiveness,
    c.activity_before_event,
    s.session_id,
    di.device_id,
    di.ble_address,
    w.worker_code,
    w.site_assigned,
    a.severity,
    a.acknowledged
FROM fall_detection.classification AS c
JOIN monitoring.sessions AS s
    ON c.session_id = s.session_id
JOIN devices.identification AS di
    ON s.device_id = di.device_id
LEFT JOIN workers.workers_list AS w
    ON s.worker_id = w.worker_id
LEFT JOIN fall_detection.alerts AS a
    ON a.classification_id = c.classification_id
WHERE c.event_type <> 'NORMAL'
ORDER BY c.ts DESC;

COMMENT ON VIEW fall_detection.v_events_detail IS
    'Dashboard-ready FALL and NEAR_MISS events joined with device, worker, and alert context.';

COMMIT;
