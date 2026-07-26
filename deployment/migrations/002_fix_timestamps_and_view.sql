-- ============================================================
-- Migration 002: fix ts column types + restore dashboard view
--
-- Run this once, after the schema.sql + devices.identification
-- migration you already applied. Safe to re-run (ALTER ... TYPE is
-- idempotent if already TIMESTAMPTZ; CREATE OR REPLACE VIEW too).
-- ============================================================

-- kafka_to_postgres.py builds timestamps as tz-aware datetimes:
--   datetime.fromtimestamp(msg["ts"] / 1000.0, tz=timezone.utc)
-- asyncpg raises on the very first insert against a bare TIMESTAMP
-- column when handed a tz-aware value -- readings flush every 50
-- samples (0.5s at 100Hz), so this fails almost immediately once
-- real traffic starts. These three columns need to be TIMESTAMPTZ.
--
-- The USING clause assumes any existing rows were written assuming
-- UTC (true for anything inserted by the current pipeline code). If
-- you've got rows from an earlier assumption, check before running.
ALTER TABLE monitoring.sessions 
ALTER COLUMN device_id TYPE TEXT;

ALTER TABLE monitoring.sessions 
ADD CONSTRAINT fk_sessions_device 
FOREIGN KEY (device_id) REFERENCES devices.identification(device_id) ON DELETE RESTRICT;

DROP VIEW IF EXISTS fall_detection.v_events_detail;

-- Fixes a real bug: readings/classification/sessions/alerts used plain
-- TIMESTAMP while devices.identification uses TIMESTAMPTZ -- mixing
-- the two caused asyncpg to reject timezone-aware Python datetimes
-- when writing from the Kafka consumer. Run this once against your
-- existing database to bring it in line.

ALTER TABLE monitoring.sessions        ALTER COLUMN start_time   TYPE TIMESTAMPTZ;
ALTER TABLE monitoring.sessions        ALTER COLUMN end_time     TYPE TIMESTAMPTZ;
ALTER TABLE monitoring.readings        ALTER COLUMN ts           TYPE TIMESTAMPTZ;
ALTER TABLE fall_detection.classification ALTER COLUMN ts        TYPE TIMESTAMPTZ;
ALTER TABLE fall_detection.classification ALTER COLUMN created_at TYPE TIMESTAMPTZ;
ALTER TABLE fall_detection.activity_log   ALTER COLUMN ts        TYPE TIMESTAMPTZ;
ALTER TABLE fall_detection.alerts         ALTER COLUMN raised_at TYPE TIMESTAMPTZ;
ALTER TABLE fall_detection.alerts         ALTER COLUMN acknowledged_at TYPE TIMESTAMPTZ;

ALTER TABLE monitoring.readings
    ALTER COLUMN ts TYPE TIMESTAMPTZ USING ts AT TIME ZONE 'UTC';

ALTER TABLE fall_detection.classification
    ALTER COLUMN ts TYPE TIMESTAMPTZ USING ts AT TIME ZONE 'UTC';

ALTER TABLE fall_detection.activity_log
    ALTER COLUMN ts TYPE TIMESTAMPTZ USING ts AT TIME ZONE 'UTC';

-- v_events_detail was dropped when devices.devices_list went away
-- (it joined on d.device_name / d.mount_location, which no longer
-- exist) and was never recreated against devices.identification.
-- Rebuilt here with the fields that still exist; add a human-readable
-- device_name column onto devices.identification if the dashboard
-- needs one -- it isn't in the current schema.
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
FROM fall_detection.classification c
JOIN monitoring.sessions s        ON c.session_id = s.session_id
JOIN devices.identification di    ON s.device_id = di.device_id
LEFT JOIN workers.workers_list w  ON s.worker_id = w.worker_id
LEFT JOIN fall_detection.alerts a ON a.classification_id = c.classification_id
WHERE c.event_type != 'NORMAL'
ORDER BY c.ts DESC;

COMMENT ON VIEW fall_detection.v_events_detail IS
    'Dashboard-ready view: every FALL/NEAR_MISS with device, worker, and alert context joined in';
