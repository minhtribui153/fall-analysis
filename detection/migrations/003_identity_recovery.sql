ALTER TABLE node_registrations
    ADD COLUMN IF NOT EXISTS claimed_uuid UUID,

    ADD COLUMN IF NOT EXISTS request_kind TEXT
        NOT NULL DEFAULT 'new';

ALTER TABLE node_registrations
    DROP CONSTRAINT IF EXISTS
        node_registrations_request_kind_check;

ALTER TABLE node_registrations
    ADD CONSTRAINT
        node_registrations_request_kind_check
    CHECK (
        request_kind IN (
            'new',
            'identity_recovery'
        )
    );

CREATE INDEX IF NOT EXISTS
    node_registrations_request_kind_idx
ON node_registrations (
    request_kind,
    approval_status,
    requested_at DESC
)
WHERE active = TRUE;