-- Current sql file was generated after introspecting the database
-- If you want to run this migration please uncomment this code before executing migrations
/*
CREATE SCHEMA "fall_detection";
--> statement-breakpoint
CREATE SCHEMA "identification";
--> statement-breakpoint
CREATE SCHEMA "monitoring";
--> statement-breakpoint
CREATE TABLE "fall_detection"."activity_log" (
	"activity_id" bigserial PRIMARY KEY,
	"session_id" uuid NOT NULL,
	"worker_id" varchar(12) NOT NULL,
	"ts" timestamp with time zone NOT NULL,
	"activity_state" varchar(20) NOT NULL,
	CONSTRAINT "activity_log_activity_state_check" CHECK (((activity_state)::text = ANY (ARRAY[('stationary'::character varying)::text, ('walking'::character varying)::text, ('running'::character varying)::text, ('jumping'::character varying)::text, ('unknown'::character varying)::text])))
);
--> statement-breakpoint
CREATE TABLE "fall_detection"."alerts" (
	"alert_id" serial PRIMARY KEY,
	"classification_id" bigint NOT NULL,
	"raised_at" timestamp with time zone DEFAULT now() NOT NULL,
	"severity" varchar(10) DEFAULT 'high' NOT NULL,
	"acknowledged" boolean DEFAULT false NOT NULL,
	"acknowledged_by" varchar(12),
	"acknowledged_at" timestamp with time zone,
	"notes" text,
	CONSTRAINT "alerts_severity_check" CHECK (((severity)::text = ANY (ARRAY[('low'::character varying)::text, ('medium'::character varying)::text, ('high'::character varying)::text])))
);
--> statement-breakpoint
CREATE TABLE "fall_detection"."classification" (
	"classification_id" bigserial PRIMARY KEY,
	"session_id" uuid NOT NULL,
	"worker_id" varchar(12) NOT NULL,
	"ts" timestamp with time zone NOT NULL,
	"event_type" varchar(20) NOT NULL,
	"fall_type" varchar(10),
	"max_smv" real,
	"max_rotation" real,
	"freefall_duration" real,
	"tilt_change" real,
	"responsiveness" varchar(10),
	"activity_before_event" varchar(20),
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "classification_event_type_check" CHECK (((event_type)::text = ANY (ARRAY[('NORMAL'::character varying)::text, ('NEAR_MISS'::character varying)::text, ('FALL'::character varying)::text]))),
	CONSTRAINT "classification_fall_type_check" CHECK ((((fall_type)::text = ANY (ARRAY[('FFH'::character varying)::text, ('STF'::character varying)::text])) OR (fall_type IS NULL))),
	CONSTRAINT "classification_responsiveness_check" CHECK ((((responsiveness)::text = ANY (ARRAY[('still'::character varying)::text, ('active'::character varying)::text])) OR (responsiveness IS NULL)))
);
--> statement-breakpoint
CREATE TABLE "identification"."devices" (
	"device_id" text PRIMARY KEY,
	"ble_address" text,
	"registered_at" timestamp with time zone DEFAULT now() NOT NULL,
	"last_seen_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE "identification"."registration_audit" (
	"audit_id" bigserial PRIMARY KEY,
	"registration_id" bigint NOT NULL,
	"device_id" text NOT NULL,
	"worker_id" varchar(12),
	"session_id" uuid,
	"previous_status" varchar(10),
	"new_status" varchar(10) NOT NULL,
	"actor" text NOT NULL,
	"reason" text,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "registration_audit_new_status_check" CHECK (((new_status)::text = ANY ((ARRAY['pending'::character varying, 'approved'::character varying, 'rejected'::character varying])::text[]))),
	CONSTRAINT "registration_audit_previous_status_check" CHECK (((previous_status IS NULL) OR ((previous_status)::text = ANY ((ARRAY['pending'::character varying, 'approved'::character varying, 'rejected'::character varying])::text[]))))
);
--> statement-breakpoint
CREATE TABLE "identification"."workers" (
	"worker_id" varchar(12) PRIMARY KEY,
	"worker_uuid" uuid NOT NULL CONSTRAINT "workers_worker_uuid_key" UNIQUE,
	"worker_name" varchar(16) NOT NULL,
	"active" boolean DEFAULT true NOT NULL,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	"updated_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "workers_worker_id_format_check" CHECK (((worker_id)::text ~ '^[A-Z0-9]{8,12}$'::text)),
	CONSTRAINT "workers_worker_name_length_check" CHECK (((char_length((worker_name)::text) >= 1) AND (char_length((worker_name)::text) <= 16)))
);
--> statement-breakpoint
CREATE TABLE "monitoring"."readings" (
	"reading_id" bigserial PRIMARY KEY,
	"session_id" uuid NOT NULL,
	"device_id" text NOT NULL,
	"sequence" bigint NOT NULL,
	"ts" timestamp with time zone NOT NULL,
	"ax" real NOT NULL,
	"ay" real NOT NULL,
	"az" real NOT NULL,
	"gx" real DEFAULT 0 NOT NULL,
	"gy" real DEFAULT 0 NOT NULL,
	"gz" real DEFAULT 0 NOT NULL,
	"smv" real,
	"gyro_magnitude" real
);
--> statement-breakpoint
CREATE TABLE "monitoring"."sessions" (
	"session_id" uuid PRIMARY KEY,
	"device_id" text NOT NULL,
	"worker_id" varchar(12) NOT NULL,
	"bearer_token_hash" text NOT NULL,
	"token_issued_at" timestamp with time zone NOT NULL,
	"token_expires_at" timestamp with time zone NOT NULL,
	"token_revoked_at" timestamp with time zone,
	"start_time" timestamp with time zone DEFAULT now() NOT NULL,
	"end_time" timestamp with time zone,
	"sample_rate_hz" integer DEFAULT 100 NOT NULL,
	"last_authenticated_at" timestamp with time zone,
	"last_auth_failure_at" timestamp with time zone,
	"active" boolean DEFAULT true NOT NULL,
	CONSTRAINT "sessions_active_state_check" CHECK ((((active = true) AND (end_time IS NULL) AND (token_revoked_at IS NULL)) OR (active = false))),
	CONSTRAINT "sessions_token_expiry_check" CHECK ((token_expires_at > token_issued_at))
);
--> statement-breakpoint
CREATE TABLE "node_registrations" (
	"registration_id" bigserial PRIMARY KEY,
	"device_id" text NOT NULL,
	"worker_id" varchar(12),
	"approval_status" varchar(10) DEFAULT 'pending' NOT NULL,
	"active" boolean DEFAULT true NOT NULL,
	"requested_at" timestamp with time zone DEFAULT now() NOT NULL,
	"registered_at" timestamp with time zone DEFAULT now() NOT NULL,
	"last_seen_at" timestamp with time zone DEFAULT now() NOT NULL,
	"decided_at" timestamp with time zone,
	"decided_by" text,
	"decision_reason" text,
	"current_session_id" uuid,
	"delinked_at" timestamp with time zone,
	CONSTRAINT "node_registrations_approval_status_check" CHECK (((approval_status)::text = ANY ((ARRAY['pending'::character varying, 'approved'::character varying, 'rejected'::character varying])::text[]))),
	CONSTRAINT "node_registrations_delink_state_check" CHECK ((((active = true) AND (delinked_at IS NULL)) OR (active = false)))
);
--> statement-breakpoint
CREATE TABLE "registration_decision_audit" (
	"audit_id" bigserial PRIMARY KEY,
	"registration_id" bigint NOT NULL,
	"device_id" text NOT NULL,
	"node_uuid" uuid NOT NULL,
	"previous_status" text NOT NULL,
	"new_status" text NOT NULL,
	"actor" text NOT NULL,
	"reason" text,
	"decided_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE "registration_schema_migrations" (
	"version" text PRIMARY KEY,
	"checksum" text NOT NULL,
	"applied_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE "registration_server_identity" (
	"singleton" boolean PRIMARY KEY DEFAULT true,
	"server_uuid" uuid NOT NULL CONSTRAINT "registration_server_identity_server_uuid_key" UNIQUE,
	"created_at" timestamp with time zone DEFAULT now() NOT NULL,
	CONSTRAINT "registration_server_identity_singleton_check" CHECK (singleton)
);
--> statement-breakpoint
CREATE INDEX "idx_activity_session_ts" ON "fall_detection"."activity_log" ("session_id","ts");--> statement-breakpoint
CREATE INDEX "idx_activity_worker_ts" ON "fall_detection"."activity_log" ("worker_id","ts");--> statement-breakpoint
CREATE INDEX "idx_alerts_unacknowledged" ON "fall_detection"."alerts" ("raised_at") WHERE (acknowledged = false);--> statement-breakpoint
CREATE INDEX "idx_classification_events" ON "fall_detection"."classification" ("event_type") WHERE ((event_type)::text <> 'NORMAL'::text);--> statement-breakpoint
CREATE INDEX "idx_classification_session_ts" ON "fall_detection"."classification" ("session_id","ts");--> statement-breakpoint
CREATE INDEX "idx_classification_worker_ts" ON "fall_detection"."classification" ("worker_id","ts");--> statement-breakpoint
CREATE INDEX "idx_registration_audit_registration" ON "identification"."registration_audit" ("registration_id","created_at" DESC);--> statement-breakpoint
CREATE INDEX "idx_registration_audit_worker" ON "identification"."registration_audit" ("worker_id","created_at" DESC);--> statement-breakpoint
CREATE UNIQUE INDEX "workers_name_unique_ci" ON "identification"."workers" (lower((worker_name)::text));--> statement-breakpoint
CREATE INDEX "idx_readings_device_ts" ON "monitoring"."readings" ("device_id","ts");--> statement-breakpoint
CREATE INDEX "idx_readings_session_ts" ON "monitoring"."readings" ("session_id","ts");--> statement-breakpoint
CREATE UNIQUE INDEX "readings_session_sequence_unique" ON "monitoring"."readings" ("session_id","sequence");--> statement-breakpoint
CREATE INDEX "idx_sessions_device_time" ON "monitoring"."sessions" ("device_id","start_time" DESC);--> statement-breakpoint
CREATE INDEX "idx_sessions_expiry" ON "monitoring"."sessions" ("token_expires_at") WHERE (active = true);--> statement-breakpoint
CREATE INDEX "idx_sessions_worker_time" ON "monitoring"."sessions" ("worker_id","start_time" DESC);--> statement-breakpoint
CREATE UNIQUE INDEX "one_active_session_per_device" ON "monitoring"."sessions" ("device_id") WHERE (active = true);--> statement-breakpoint
CREATE UNIQUE INDEX "one_active_session_per_worker" ON "monitoring"."sessions" ("worker_id") WHERE (active = true);--> statement-breakpoint
CREATE UNIQUE INDEX "sessions_session_device_unique" ON "monitoring"."sessions" ("session_id","device_id");--> statement-breakpoint
CREATE UNIQUE INDEX "sessions_session_worker_unique" ON "monitoring"."sessions" ("session_id","worker_id");--> statement-breakpoint
CREATE INDEX "idx_node_registrations_status" ON "node_registrations" ("approval_status","requested_at" DESC) WHERE (active = true);--> statement-breakpoint
CREATE INDEX "idx_node_registrations_worker" ON "node_registrations" ("worker_id","active");--> statement-breakpoint
CREATE UNIQUE INDEX "one_active_registration_per_device" ON "node_registrations" ("device_id") WHERE (active = true);--> statement-breakpoint
CREATE INDEX "registration_decision_audit_registration_idx" ON "registration_decision_audit" ("registration_id","decided_at" DESC);--> statement-breakpoint
ALTER TABLE "node_registrations" ADD CONSTRAINT "node_registrations_current_session_fkey" FOREIGN KEY ("current_session_id") REFERENCES "monitoring"."sessions"("session_id") ON DELETE SET NULL;--> statement-breakpoint
ALTER TABLE "node_registrations" ADD CONSTRAINT "node_registrations_device_fkey" FOREIGN KEY ("device_id") REFERENCES "identification"."devices"("device_id") ON DELETE RESTRICT;--> statement-breakpoint
ALTER TABLE "node_registrations" ADD CONSTRAINT "node_registrations_worker_fkey" FOREIGN KEY ("worker_id") REFERENCES "identification"."workers"("worker_id") ON DELETE SET NULL;--> statement-breakpoint
ALTER TABLE "monitoring"."sessions" ADD CONSTRAINT "sessions_device_fkey" FOREIGN KEY ("device_id") REFERENCES "identification"."devices"("device_id") ON DELETE RESTRICT;--> statement-breakpoint
ALTER TABLE "monitoring"."sessions" ADD CONSTRAINT "sessions_worker_fkey" FOREIGN KEY ("worker_id") REFERENCES "identification"."workers"("worker_id") ON DELETE RESTRICT;--> statement-breakpoint
ALTER TABLE "identification"."registration_audit" ADD CONSTRAINT "registration_audit_registration_fkey" FOREIGN KEY ("registration_id") REFERENCES "node_registrations"("registration_id") ON DELETE CASCADE;--> statement-breakpoint
ALTER TABLE "identification"."registration_audit" ADD CONSTRAINT "registration_audit_session_fkey" FOREIGN KEY ("session_id") REFERENCES "monitoring"."sessions"("session_id") ON DELETE SET NULL;--> statement-breakpoint
ALTER TABLE "identification"."registration_audit" ADD CONSTRAINT "registration_audit_worker_fkey" FOREIGN KEY ("worker_id") REFERENCES "identification"."workers"("worker_id") ON DELETE SET NULL;--> statement-breakpoint
ALTER TABLE "monitoring"."readings" ADD CONSTRAINT "readings_session_device_fkey" FOREIGN KEY ("session_id","device_id") REFERENCES "monitoring"."sessions"("session_id","device_id") ON DELETE CASCADE;--> statement-breakpoint
ALTER TABLE "fall_detection"."activity_log" ADD CONSTRAINT "activity_log_session_worker_fkey" FOREIGN KEY ("session_id","worker_id") REFERENCES "monitoring"."sessions"("session_id","worker_id") ON DELETE CASCADE;--> statement-breakpoint
ALTER TABLE "fall_detection"."classification" ADD CONSTRAINT "classification_session_worker_fkey" FOREIGN KEY ("session_id","worker_id") REFERENCES "monitoring"."sessions"("session_id","worker_id") ON DELETE CASCADE;--> statement-breakpoint
ALTER TABLE "fall_detection"."alerts" ADD CONSTRAINT "alerts_acknowledged_by_worker_fkey" FOREIGN KEY ("acknowledged_by") REFERENCES "identification"."workers"("worker_id") ON DELETE SET NULL;--> statement-breakpoint
ALTER TABLE "fall_detection"."alerts" ADD CONSTRAINT "alerts_classification_fkey" FOREIGN KEY ("classification_id") REFERENCES "fall_detection"."classification"("classification_id") ON DELETE CASCADE;--> statement-breakpoint
CREATE VIEW "fall_detection"."v_events_detail" AS (SELECT c.classification_id, c.ts, c.event_type, c.fall_type, c.max_smv, c.max_rotation, c.freefall_duration, c.tilt_change, c.responsiveness, c.activity_before_event, c.session_id, c.worker_id, w.worker_name, w.worker_uuid, s.device_id, d.ble_address, s.start_time AS session_start_time, s.end_time AS session_end_time, s.token_expires_at, a.severity, a.acknowledged, a.acknowledged_at FROM fall_detection.classification c JOIN identification.workers w ON c.worker_id::text = w.worker_id::text JOIN monitoring.sessions s ON c.session_id = s.session_id AND c.worker_id::text = s.worker_id::text JOIN identification.devices d ON s.device_id = d.device_id LEFT JOIN fall_detection.alerts a ON a.classification_id = c.classification_id WHERE c.event_type::text <> 'NORMAL'::text ORDER BY c.ts DESC);
*/