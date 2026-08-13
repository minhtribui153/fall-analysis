import { pgSchema, pgTable, uuid, bigserial, serial, varchar, text, boolean, bigint, timestamp, real, integer, index, uniqueIndex, foreignKey, primaryKey, unique, check } from "drizzle-orm/pg-core"
import { sql } from "drizzle-orm"

export const fallDetection = pgSchema("fall_detection");
export const identification = pgSchema("identification");
export const monitoring = pgSchema("monitoring");


export const activityLogInFallDetection = fallDetection.table("activity_log", {
	activityId: bigserial("activity_id", { mode: 'number' }).primaryKey(),
	sessionId: uuid("session_id").notNull(),
	workerId: varchar("worker_id", { length: 12 }).notNull(),
	ts: timestamp({ withTimezone: true }).notNull(),
	activityState: varchar("activity_state", { length: 20 }).notNull(),
}, (table) => [
	foreignKey({
		columns: [table.sessionId, table.workerId],
		foreignColumns: [sessionsInMonitoring.sessionId, sessionsInMonitoring.workerId],
		name: "activity_log_session_worker_fkey"
	}).onDelete("cascade"),
	index("idx_activity_session_ts").using("btree", table.sessionId.asc().nullsLast(), table.ts.asc().nullsLast()),
	index("idx_activity_worker_ts").using("btree", table.workerId.asc().nullsLast(), table.ts.asc().nullsLast()),
check("activity_log_activity_state_check", sql`((activity_state)::text = ANY (ARRAY[('stationary'::character varying)::text, ('walking'::character varying)::text, ('running'::character varying)::text, ('jumping'::character varying)::text, ('unknown'::character varying)::text]))`),]);

export const alertsInFallDetection = fallDetection.table("alerts", {
	alertId: serial("alert_id").primaryKey(),
	classificationId: bigint("classification_id", { mode: 'number' }).notNull().references(() => classificationInFallDetection.classificationId, { onDelete: "cascade" } ),
	raisedAt: timestamp("raised_at", { withTimezone: true }).default(sql`now()`).notNull(),
	severity: varchar({ length: 10 }).default("high").notNull(),
	acknowledged: boolean().default(false).notNull(),
	acknowledgedBy: varchar("acknowledged_by", { length: 12 }).references(() => workersInIdentification.workerId, { onDelete: "set null" } ),
	acknowledgedAt: timestamp("acknowledged_at", { withTimezone: true }),
	notes: text(),
}, (table) => [
	index("idx_alerts_unacknowledged").using("btree", table.raisedAt.asc().nullsLast()).where(sql`(acknowledged = false)`),
check("alerts_severity_check", sql`((severity)::text = ANY (ARRAY[('low'::character varying)::text, ('medium'::character varying)::text, ('high'::character varying)::text]))`),]);

export const classificationInFallDetection = fallDetection.table("classification", {
	classificationId: bigserial("classification_id", { mode: 'number' }).primaryKey(),
	sessionId: uuid("session_id").notNull(),
	workerId: varchar("worker_id", { length: 12 }).notNull(),
	ts: timestamp({ withTimezone: true }).notNull(),
	eventType: varchar("event_type", { length: 20 }).notNull(),
	fallType: varchar("fall_type", { length: 10 }),
	maxSmv: real("max_smv"),
	maxRotation: real("max_rotation"),
	freefallDuration: real("freefall_duration"),
	tiltChange: real("tilt_change"),
	responsiveness: varchar({ length: 10 }),
	activityBeforeEvent: varchar("activity_before_event", { length: 20 }),
	createdAt: timestamp("created_at", { withTimezone: true }).default(sql`now()`).notNull(),
}, (table) => [
	foreignKey({
		columns: [table.sessionId, table.workerId],
		foreignColumns: [sessionsInMonitoring.sessionId, sessionsInMonitoring.workerId],
		name: "classification_session_worker_fkey"
	}).onDelete("cascade"),
	index("idx_classification_events").using("btree", table.eventType.asc().nullsLast()).where(sql`((event_type)::text <> 'NORMAL'::text)`),
	index("idx_classification_session_ts").using("btree", table.sessionId.asc().nullsLast(), table.ts.asc().nullsLast()),
	index("idx_classification_worker_ts").using("btree", table.workerId.asc().nullsLast(), table.ts.asc().nullsLast()),
check("classification_event_type_check", sql`((event_type)::text = ANY (ARRAY[('NORMAL'::character varying)::text, ('NEAR_MISS'::character varying)::text, ('FALL'::character varying)::text]))`),check("classification_fall_type_check", sql`(((fall_type)::text = ANY (ARRAY[('FFH'::character varying)::text, ('STF'::character varying)::text])) OR (fall_type IS NULL))`),check("classification_responsiveness_check", sql`(((responsiveness)::text = ANY (ARRAY[('still'::character varying)::text, ('active'::character varying)::text])) OR (responsiveness IS NULL))`),]);

export const devicesInIdentification = identification.table("devices", {
	deviceId: text("device_id").primaryKey(),
	bleAddress: text("ble_address"),
	registeredAt: timestamp("registered_at", { withTimezone: true }).default(sql`now()`).notNull(),
	lastSeenAt: timestamp("last_seen_at", { withTimezone: true }).default(sql`now()`).notNull(),
});

export const registrationAuditInIdentification = identification.table("registration_audit", {
	auditId: bigserial("audit_id", { mode: 'number' }).primaryKey(),
	registrationId: bigint("registration_id", { mode: 'number' }).notNull().references(() => nodeRegistrations.registrationId, { onDelete: "cascade" } ),
	deviceId: text("device_id").notNull(),
	workerId: varchar("worker_id", { length: 12 }).references(() => workersInIdentification.workerId, { onDelete: "set null" } ),
	sessionId: uuid("session_id").references(() => sessionsInMonitoring.sessionId, { onDelete: "set null" } ),
	previousStatus: varchar("previous_status", { length: 10 }),
	newStatus: varchar("new_status", { length: 10 }).notNull(),
	actor: text().notNull(),
	reason: text(),
	createdAt: timestamp("created_at", { withTimezone: true }).default(sql`now()`).notNull(),
}, (table) => [
	index("idx_registration_audit_registration").using("btree", table.registrationId.asc().nullsLast(), table.createdAt.desc().nullsFirst()),
	index("idx_registration_audit_worker").using("btree", table.workerId.asc().nullsLast(), table.createdAt.desc().nullsFirst()),
check("registration_audit_new_status_check", sql`((new_status)::text = ANY ((ARRAY['pending'::character varying, 'approved'::character varying, 'rejected'::character varying])::text[]))`),check("registration_audit_previous_status_check", sql`((previous_status IS NULL) OR ((previous_status)::text = ANY ((ARRAY['pending'::character varying, 'approved'::character varying, 'rejected'::character varying])::text[])))`),]);

export const workersInIdentification = identification.table("workers", {
	workerId: varchar("worker_id", { length: 12 }).primaryKey(),
	workerUuid: uuid("worker_uuid").notNull(),
	workerName: varchar("worker_name", { length: 16 }).notNull(),
	active: boolean().default(true).notNull(),
	createdAt: timestamp("created_at", { withTimezone: true }).default(sql`now()`).notNull(),
	updatedAt: timestamp("updated_at", { withTimezone: true }).default(sql`now()`).notNull(),
}, (table) => [
	uniqueIndex("workers_name_unique_ci").using("btree", sql`lower((worker_name)::text)`),
	unique("workers_worker_uuid_key").on(table.workerUuid),check("workers_worker_id_format_check", sql`((worker_id)::text ~ '^[A-Z0-9]{8,12}$'::text)`),check("workers_worker_name_length_check", sql`((char_length((worker_name)::text) >= 1) AND (char_length((worker_name)::text) <= 16))`),]);

export const readingsInMonitoring = monitoring.table("readings", {
	readingId: bigserial("reading_id", { mode: 'number' }).primaryKey(),
	sessionId: uuid("session_id").notNull(),
	deviceId: text("device_id").notNull(),
	sequence: bigint({ mode: 'number' }).notNull(),
	ts: timestamp({ withTimezone: true }).notNull(),
	ax: real().notNull(),
	ay: real().notNull(),
	az: real().notNull(),
	gx: real().default(0).notNull(),
	gy: real().default(0).notNull(),
	gz: real().default(0).notNull(),
	smv: real(),
	gyroMagnitude: real("gyro_magnitude"),
}, (table) => [
	foreignKey({
		columns: [table.sessionId, table.deviceId],
		foreignColumns: [sessionsInMonitoring.sessionId, sessionsInMonitoring.deviceId],
		name: "readings_session_device_fkey"
	}).onDelete("cascade"),
	index("idx_readings_device_ts").using("btree", table.deviceId.asc().nullsLast(), table.ts.asc().nullsLast()),
	index("idx_readings_session_ts").using("btree", table.sessionId.asc().nullsLast(), table.ts.asc().nullsLast()),
	uniqueIndex("readings_session_sequence_unique").using("btree", table.sessionId.asc().nullsLast(), table.sequence.asc().nullsLast()),
]);

export const sessionsInMonitoring = monitoring.table("sessions", {
	sessionId: uuid("session_id").primaryKey(),
	deviceId: text("device_id").notNull().references(() => devicesInIdentification.deviceId, { onDelete: "restrict" } ),
	workerId: varchar("worker_id", { length: 12 }).notNull().references(() => workersInIdentification.workerId, { onDelete: "restrict" } ),
	bearerTokenHash: text("bearer_token_hash").notNull(),
	tokenIssuedAt: timestamp("token_issued_at", { withTimezone: true }).notNull(),
	tokenExpiresAt: timestamp("token_expires_at", { withTimezone: true }).notNull(),
	tokenRevokedAt: timestamp("token_revoked_at", { withTimezone: true }),
	startTime: timestamp("start_time", { withTimezone: true }).default(sql`now()`).notNull(),
	endTime: timestamp("end_time", { withTimezone: true }),
	sampleRateHz: integer("sample_rate_hz").default(100).notNull(),
	lastAuthenticatedAt: timestamp("last_authenticated_at", { withTimezone: true }),
	lastAuthFailureAt: timestamp("last_auth_failure_at", { withTimezone: true }),
	active: boolean().default(true).notNull(),
}, (table) => [
	index("idx_sessions_device_time").using("btree", table.deviceId.asc().nullsLast(), table.startTime.desc().nullsFirst()),
	index("idx_sessions_expiry").using("btree", table.tokenExpiresAt.asc().nullsLast()).where(sql`(active = true)`),
	index("idx_sessions_worker_time").using("btree", table.workerId.asc().nullsLast(), table.startTime.desc().nullsFirst()),
	uniqueIndex("one_active_session_per_device").using("btree", table.deviceId.asc().nullsLast()).where(sql`(active = true)`),
	uniqueIndex("one_active_session_per_worker").using("btree", table.workerId.asc().nullsLast()).where(sql`(active = true)`),
	uniqueIndex("sessions_session_device_unique").using("btree", table.sessionId.asc().nullsLast(), table.deviceId.asc().nullsLast()),
	uniqueIndex("sessions_session_worker_unique").using("btree", table.sessionId.asc().nullsLast(), table.workerId.asc().nullsLast()),
check("sessions_active_state_check", sql`(((active = true) AND (end_time IS NULL) AND (token_revoked_at IS NULL)) OR (active = false))`),check("sessions_token_expiry_check", sql`(token_expires_at > token_issued_at)`),]);

export const nodeRegistrations = pgTable("node_registrations", {
	registrationId: bigserial("registration_id", { mode: 'number' }).primaryKey(),
	deviceId: text("device_id").notNull().references(() => devicesInIdentification.deviceId, { onDelete: "restrict" } ),
	workerId: varchar("worker_id", { length: 12 }).references(() => workersInIdentification.workerId, { onDelete: "set null" } ),
	approvalStatus: varchar("approval_status", { length: 10 }).default("pending").notNull(),
	active: boolean().default(true).notNull(),
	requestedAt: timestamp("requested_at", { withTimezone: true }).default(sql`now()`).notNull(),
	registeredAt: timestamp("registered_at", { withTimezone: true }).default(sql`now()`).notNull(),
	lastSeenAt: timestamp("last_seen_at", { withTimezone: true }).default(sql`now()`).notNull(),
	decidedAt: timestamp("decided_at", { withTimezone: true }),
	decidedBy: text("decided_by"),
	decisionReason: text("decision_reason"),
	currentSessionId: uuid("current_session_id").references(() => sessionsInMonitoring.sessionId, { onDelete: "set null" } ),
	delinkedAt: timestamp("delinked_at", { withTimezone: true }),
}, (table) => [
	index("idx_node_registrations_status").using("btree", table.approvalStatus.asc().nullsLast(), table.requestedAt.desc().nullsFirst()).where(sql`(active = true)`),
	index("idx_node_registrations_worker").using("btree", table.workerId.asc().nullsLast(), table.active.asc().nullsLast()),
	uniqueIndex("one_active_registration_per_device").using("btree", table.deviceId.asc().nullsLast()).where(sql`(active = true)`),
check("node_registrations_approval_status_check", sql`((approval_status)::text = ANY ((ARRAY['pending'::character varying, 'approved'::character varying, 'rejected'::character varying])::text[]))`),check("node_registrations_delink_state_check", sql`(((active = true) AND (delinked_at IS NULL)) OR (active = false))`),]);

export const registrationDecisionAudit = pgTable("registration_decision_audit", {
	auditId: bigserial("audit_id", { mode: 'number' }).primaryKey(),
	registrationId: bigint("registration_id", { mode: 'number' }).notNull(),
	deviceId: text("device_id").notNull(),
	nodeUuid: uuid("node_uuid").notNull(),
	previousStatus: text("previous_status").notNull(),
	newStatus: text("new_status").notNull(),
	actor: text().notNull(),
	reason: text(),
	decidedAt: timestamp("decided_at", { withTimezone: true }).default(sql`now()`).notNull(),
}, (table) => [
	index("registration_decision_audit_registration_idx").using("btree", table.registrationId.asc().nullsLast(), table.decidedAt.desc().nullsFirst()),
]);

export const registrationSchemaMigrations = pgTable("registration_schema_migrations", {
	version: text().primaryKey(),
	checksum: text().notNull(),
	appliedAt: timestamp("applied_at", { withTimezone: true }).default(sql`now()`).notNull(),
});

export const registrationServerIdentity = pgTable("registration_server_identity", {
	singleton: boolean().default(true).primaryKey(),
	serverUuid: uuid("server_uuid").notNull(),
	createdAt: timestamp("created_at", { withTimezone: true }).default(sql`now()`).notNull(),
}, (table) => [
	unique("registration_server_identity_server_uuid_key").on(table.serverUuid),check("registration_server_identity_singleton_check", sql`singleton`),]);
export const vEventsDetailInFallDetection = fallDetection.view("v_events_detail", {	classificationId: bigint("classification_id", { mode: 'number' }),
	ts: timestamp({ withTimezone: true }),
	eventType: varchar("event_type", { length: 20 }),
	fallType: varchar("fall_type", { length: 10 }),
	maxSmv: real("max_smv"),
	maxRotation: real("max_rotation"),
	freefallDuration: real("freefall_duration"),
	tiltChange: real("tilt_change"),
	responsiveness: varchar({ length: 10 }),
	activityBeforeEvent: varchar("activity_before_event", { length: 20 }),
	sessionId: uuid("session_id"),
	workerId: varchar("worker_id", { length: 12 }),
	workerName: varchar("worker_name", { length: 16 }),
	workerUuid: uuid("worker_uuid"),
	deviceId: text("device_id"),
	bleAddress: text("ble_address"),
	sessionStartTime: timestamp("session_start_time", { withTimezone: true }),
	sessionEndTime: timestamp("session_end_time", { withTimezone: true }),
	tokenExpiresAt: timestamp("token_expires_at", { withTimezone: true }),
	severity: varchar({ length: 10 }),
	acknowledged: boolean(),
	acknowledgedAt: timestamp("acknowledged_at", { withTimezone: true }),
}).as(sql`SELECT c.classification_id, c.ts, c.event_type, c.fall_type, c.max_smv, c.max_rotation, c.freefall_duration, c.tilt_change, c.responsiveness, c.activity_before_event, c.session_id, c.worker_id, w.worker_name, w.worker_uuid, s.device_id, d.ble_address, s.start_time AS session_start_time, s.end_time AS session_end_time, s.token_expires_at, a.severity, a.acknowledged, a.acknowledged_at FROM fall_detection.classification c JOIN identification.workers w ON c.worker_id::text = w.worker_id::text JOIN monitoring.sessions s ON c.session_id = s.session_id AND c.worker_id::text = s.worker_id::text JOIN identification.devices d ON s.device_id = d.device_id LEFT JOIN fall_detection.alerts a ON a.classification_id = c.classification_id WHERE c.event_type::text <> 'NORMAL'::text ORDER BY c.ts DESC`);