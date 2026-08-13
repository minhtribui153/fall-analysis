ALTER TABLE node_registrations
    ADD COLUMN IF NOT EXISTS approval_status TEXT
        NOT NULL DEFAULT 'pending',

    ADD COLUMN IF NOT EXISTS requested_at TIMESTAMPTZ
        NOT NULL DEFAULT now(),

    ADD COLUMN IF NOT EXISTS decided_at TIMESTAMPTZ,

    ADD COLUMN IF NOT EXISTS decided_by TEXT,

    ADD COLUMN IF NOT EXISTS decision_reason TEXT,

    ADD COLUMN IF NOT EXISTS revision BIGINT
        NOT NULL DEFAULT 1;

ALTER TABLE node_registrations
    DROP CONSTRAINT IF EXISTS
        node_registrations_approval_status_check;

ALTER TABLE node_registrations
    ADD CONSTRAINT
        node_registrations_approval_status_check
    CHECK (
        approval_status IN (
            'pending',
            'approved',
            'rejected'
        )
    );

-- Existing active registrations existed before the approval workflow.
-- Preserve them as already approved.
UPDATE node_registrations
SET approval_status = 'approved',
    decided_at = COALESCE(
        decided_at,
        registered_at
    ),
    decided_by = COALESCE(
        decided_by,
        'migration'
    )
WHERE active = TRUE
  AND approval_status = 'pending';

CREATE INDEX IF NOT EXISTS
    node_registrations_status_idx
ON node_registrations (
    approval_status,
    requested_at DESC
)
WHERE active = TRUE;

CREATE TABLE IF NOT EXISTS
    registration_decision_audit (
        audit_id BIGSERIAL PRIMARY KEY,

        registration_id BIGINT NOT NULL
            REFERENCES node_registrations(
                registration_id
            )
            ON DELETE RESTRICT,

        device_id TEXT NOT NULL,
        node_uuid UUID NOT NULL,

        previous_status TEXT NOT NULL,
        new_status TEXT NOT NULL,

        actor TEXT NOT NULL,
        reason TEXT,

        decided_at TIMESTAMPTZ
            NOT NULL DEFAULT now()
    );

CREATE INDEX IF NOT EXISTS
    registration_decision_audit_registration_idx
ON registration_decision_audit (
    registration_id,
    decided_at DESC
);

ALTER TABLE node_registrations
    ADD COLUMN IF NOT EXISTS claimed_uuid UUID,

    ADD COLUMN IF NOT EXISTS request_kind TEXT
        NOT NULL DEFAULT 'new'
        CHECK (
            request_kind IN (
                'new',
                'identity_recovery'
            )
        );