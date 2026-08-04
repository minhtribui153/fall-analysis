CREATE SCHEMA IF NOT EXISTS "devices";
--> statement-breakpoint
CREATE SCHEMA IF NOT EXISTS "fall_detection";
--> statement-breakpoint
CREATE SCHEMA IF NOT EXISTS "monitoring";
--> statement-breakpoint
CREATE SCHEMA IF NOT EXISTS "workers";
--> statement-breakpoint
CREATE TABLE IF NOT EXISTS "fall_detection"."activity_log" (
	"activity_id" bigserial PRIMARY KEY,
	"session_id" integer NOT NULL,
	"ts" timestamp with time zone NOT NULL,
	"activity_state" varchar(20) NOT NULL,
	CONSTRAINT "activity_log_activity_state_check" CHECK (((activity_state)::text = ANY ((ARRAY['stationary'::character varying, 'walking'::character varying, 'running'::character varying, 'jumping'::character varying, 'unknown'::character varying])::text[])))
);
--> statement-breakpoint
CREATE TABLE IF NOT EXISTS "fall_detection"."alerts" (
	"alert_id" serial PRIMARY KEY,
	"classification_id" bigint NOT NULL,
	"raised_at" timestamp with time zone DEFAULT now() NOT NULL,
	"severity" varchar(10) DEFAULT 'high' NOT NULL,
	"acknowledged" boolean DEFAULT false NOT NULL,
	"acknowledged_by" integer,
	"acknowledged_at" timestamp with time zone,
	"notes" text,
	CONSTRAINT "alerts_severity_check" CHECK (((severity)::text = ANY ((ARRAY['low'::character varying, 'medium'::character varying, 'high'::character varying])::text[])))
);
--> statement-breakpoint
CREATE TABLE IF NOT EXISTS "fall_detection"."classification" (
	"classification_id" bigserial PRIMARY KEY,
	"session_id" integer NOT NULL,
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
	CONSTRAINT "classification_event_type_check" CHECK (((event_type)::text = ANY ((ARRAY['NORMAL'::character varying, 'NEAR_MISS'::character varying, 'FALL'::character varying])::text[]))),
	CONSTRAINT "classification_fall_type_check" CHECK ((((fall_type)::text = ANY ((ARRAY['FFH'::character varying, 'STF'::character varying])::text[])) OR (fall_type IS NULL))),
	CONSTRAINT "classification_responsiveness_check" CHECK ((((responsiveness)::text = ANY ((ARRAY['still'::character varying, 'active'::character varying])::text[])) OR (responsiveness IS NULL)))
);
--> statement-breakpoint
CREATE TABLE IF NOT EXISTS "devices"."identification" (
	"device_id" text PRIMARY KEY,
	"node_uuid" uuid NOT NULL CONSTRAINT "identification_node_uuid_key" UNIQUE,
	"ble_address" text,
	"registered_at" timestamp with time zone DEFAULT now() NOT NULL,
	"last_seen_at" timestamp with time zone DEFAULT now() NOT NULL
);
--> statement-breakpoint
CREATE TABLE IF NOT EXISTS "node_registrations" (
	"registration_id" bigserial PRIMARY KEY,
	"hardware_id" bytea NOT NULL,
	"node_uuid" uuid NOT NULL CONSTRAINT "node_registrations_node_uuid_key" UNIQUE,
	"active" boolean DEFAULT true NOT NULL,
	"registered_at" timestamp with time zone DEFAULT now() NOT NULL,
	"last_seen_at" timestamp with time zone DEFAULT now() NOT NULL,
	"delinked_at" timestamp with time zone,
	CONSTRAINT "node_registrations_hardware_id_check" CHECK ((octet_length(hardware_id) = 6))
);
--> statement-breakpoint
CREATE TABLE IF NOT EXISTS "monitoring"."readings" (
	"reading_id" bigserial PRIMARY KEY,
	"session_id" integer NOT NULL,
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
CREATE TABLE IF NOT EXISTS "monitoring"."sessions" (
	"session_id" serial PRIMARY KEY,
	"device_id" text NOT NULL,
	"worker_id" integer,
	"start_time" timestamp with time zone DEFAULT now() NOT NULL,
	"end_time" timestamp with time zone,
	"sample_rate_hz" integer DEFAULT 100 NOT NULL
);
--> statement-breakpoint
CREATE TABLE IF NOT EXISTS "workers"."workers_list" (
	"worker_id" serial PRIMARY KEY,
	"worker_code" varchar(20) NOT NULL CONSTRAINT "workers_list_worker_code_key" UNIQUE,
	"site_assigned" varchar(100),
	"role" varchar(50),
	"status" varchar(20) DEFAULT 'active' NOT NULL,
	"created_at" timestamp DEFAULT now() NOT NULL,
	CONSTRAINT "workers_list_status_check" CHECK (((status)::text = ANY ((ARRAY['active'::character varying, 'inactive'::character varying])::text[])))
);
--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_activity_session_ts" ON "fall_detection"."activity_log" ("session_id","ts");--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_alerts_unacknowledged" ON "fall_detection"."alerts" ("raised_at") WHERE (acknowledged = false);--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_classification_events" ON "fall_detection"."classification" ("event_type") WHERE ((event_type)::text <> 'NORMAL'::text);--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_classification_session_ts" ON "fall_detection"."classification" ("session_id","ts");--> statement-breakpoint
CREATE UNIQUE INDEX IF NOT EXISTS "one_active_uuid_per_hardware" ON "node_registrations" ("hardware_id") WHERE (active = true);--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_readings_session_ts" ON "monitoring"."readings" ("session_id","ts");--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_sessions_device" ON "monitoring"."sessions" ("device_id");--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_sessions_open" ON "monitoring"."sessions" ("device_id") WHERE (end_time IS NULL);--> statement-breakpoint
CREATE INDEX IF NOT EXISTS "idx_sessions_worker" ON "monitoring"."sessions" ("worker_id");--> statement-breakpoint
DO $$ BEGIN
	IF NOT EXISTS (
		SELECT 1
		FROM pg_constraint
		WHERE conname = 'activity_log_session_id_sessions_session_id_fkey'
	) THEN
		ALTER TABLE "fall_detection"."activity_log"
		ADD CONSTRAINT "activity_log_session_id_sessions_session_id_fkey"
		FOREIGN KEY ("session_id") REFERENCES "monitoring"."sessions"("session_id") ON DELETE CASCADE;
	END IF;
END $$;--> statement-breakpoint
DO $$ BEGIN
	IF NOT EXISTS (
		SELECT 1
		FROM pg_constraint
		WHERE conname = 'alerts_classification_id_classification_classification_id_fkey'
	) THEN
		ALTER TABLE "fall_detection"."alerts"
		ADD CONSTRAINT "alerts_classification_id_classification_classification_id_fkey"
		FOREIGN KEY ("classification_id") REFERENCES "fall_detection"."classification"("classification_id") ON DELETE CASCADE;
	END IF;
END $$;--> statement-breakpoint
DO $$ BEGIN
	IF NOT EXISTS (
		SELECT 1
		FROM pg_constraint
		WHERE conname = 'alerts_acknowledged_by_workers_list_worker_id_fkey'
	) THEN
		ALTER TABLE "fall_detection"."alerts"
		ADD CONSTRAINT "alerts_acknowledged_by_workers_list_worker_id_fkey"
		FOREIGN KEY ("acknowledged_by") REFERENCES "workers"."workers_list"("worker_id") ON DELETE SET NULL;
	END IF;
END $$;--> statement-breakpoint
DO $$ BEGIN
	IF NOT EXISTS (
		SELECT 1
		FROM pg_constraint
		WHERE conname = 'classification_session_id_sessions_session_id_fkey'
	) THEN
		ALTER TABLE "fall_detection"."classification"
		ADD CONSTRAINT "classification_session_id_sessions_session_id_fkey"
		FOREIGN KEY ("session_id") REFERENCES "monitoring"."sessions"("session_id") ON DELETE CASCADE;
	END IF;
END $$;--> statement-breakpoint
DO $$ BEGIN
	IF NOT EXISTS (
		SELECT 1
		FROM pg_constraint
		WHERE conname = 'readings_session_id_sessions_session_id_fkey'
	) THEN
		ALTER TABLE "monitoring"."readings"
		ADD CONSTRAINT "readings_session_id_sessions_session_id_fkey"
		FOREIGN KEY ("session_id") REFERENCES "monitoring"."sessions"("session_id") ON DELETE CASCADE;
	END IF;
END $$;--> statement-breakpoint
DO $$ BEGIN
	IF NOT EXISTS (
		SELECT 1
		FROM pg_constraint
		WHERE conname = 'sessions_device_id_identification_device_id_fkey'
	) THEN
		ALTER TABLE "monitoring"."sessions"
		ADD CONSTRAINT "sessions_device_id_identification_device_id_fkey"
		FOREIGN KEY ("device_id") REFERENCES "devices"."identification"("device_id") ON DELETE RESTRICT;
	END IF;
END $$;--> statement-breakpoint
DO $$ BEGIN
	IF NOT EXISTS (
		SELECT 1
		FROM pg_constraint
		WHERE conname = 'sessions_worker_id_workers_list_worker_id_fkey'
	) THEN
		ALTER TABLE "monitoring"."sessions"
		ADD CONSTRAINT "sessions_worker_id_workers_list_worker_id_fkey"
		FOREIGN KEY ("worker_id") REFERENCES "workers"."workers_list"("worker_id") ON DELETE SET NULL;
	END IF;
END $$;--> statement-breakpoint
CREATE OR REPLACE VIEW "fall_detection"."v_events_detail" AS (SELECT c.classification_id, c.ts, c.event_type, c.fall_type, c.max_smv, c.max_rotation, c.responsiveness, c.activity_before_event, s.session_id, di.device_id, di.ble_address, w.worker_code, w.site_assigned, a.severity, a.acknowledged FROM fall_detection.classification c JOIN monitoring.sessions s ON c.session_id = s.session_id JOIN devices.identification di ON s.device_id = di.device_id LEFT JOIN workers.workers_list w ON s.worker_id = w.worker_id LEFT JOIN fall_detection.alerts a ON a.classification_id = c.classification_id WHERE c.event_type::text <> 'NORMAL'::text ORDER BY c.ts DESC);