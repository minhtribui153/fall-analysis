import { pgSchema, pgTable, bigserial, serial, text, customType, integer, varchar, uuid, bigint, timestamp, real, boolean, index, uniqueIndex, foreignKey, primaryKey, unique, check } from "drizzle-orm/pg-core"
import { sql } from "drizzle-orm"

export const devices = pgSchema("devices");
export const fallDetection = pgSchema("fall_detection");
export const monitoring = pgSchema("monitoring");
export const workers = pgSchema("workers");


export const identificationInDevices = devices.table("identification", {
	deviceId: text("device_id").primaryKey(),
	nodeUuid: uuid("node_uuid").notNull(),
	bleAddress: text("ble_address"),
	registeredAt: timestamp("registered_at", { withTimezone: true }).default(sql`now()`).notNull(),
	lastSeenAt: timestamp("last_seen_at", { withTimezone: true }).default(sql`now()`).notNull(),
}, (table) => [
	unique("identification_node_uuid_key").on(table.nodeUuid),]);

export const activityLogInFallDetection = fallDetection.table("activity_log", {
	activityId: bigserial("activity_id", { mode: 'number' }).primaryKey(),
	sessionId: integer("session_id").notNull().references(() => sessionsInMonitoring.sessionId, { onDelete: "cascade" } ),
	ts: timestamp({ withTimezone: true }).notNull(),
	activityState: varchar("activity_state", { length: 20 }).notNull(),
}, (table) => [
	index("idx_activity_session_ts").using("btree", table.sessionId.asc().nullsLast(), table.ts.asc().nullsLast()),
check("activity_log_activity_state_check", sql`((activity_state)::text = ANY ((ARRAY['stationary'::character varying, 'walking'::character varying, 'running'::character varying, 'jumping'::character varying, 'unknown'::character varying])::text[]))`),]);

export const alertsInFallDetection = fallDetection.table("alerts", {
	alertId: serial("alert_id").primaryKey(),
	classificationId: bigint("classification_id", { mode: 'number' }).notNull().references(() => classificationInFallDetection.classificationId, { onDelete: "cascade" } ),
	raisedAt: timestamp("raised_at", { withTimezone: true }).default(sql`now()`).notNull(),
	severity: varchar({ length: 10 }).default("high").notNull(),
	acknowledged: boolean().default(false).notNull(),
	acknowledgedBy: integer("acknowledged_by").references(() => workersListInWorkers.workerId, { onDelete: "set null" } ),
	acknowledgedAt: timestamp("acknowledged_at", { withTimezone: true }),
	notes: text(),
}, (table) => [
	index("idx_alerts_unacknowledged").using("btree", table.raisedAt.asc().nullsLast()).where(sql`(acknowledged = false)`),
check("alerts_severity_check", sql`((severity)::text = ANY ((ARRAY['low'::character varying, 'medium'::character varying, 'high'::character varying])::text[]))`),]);

export const classificationInFallDetection = fallDetection.table("classification", {
	classificationId: bigserial("classification_id", { mode: 'number' }).primaryKey(),
	sessionId: integer("session_id").notNull().references(() => sessionsInMonitoring.sessionId, { onDelete: "cascade" } ),
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
	index("idx_classification_events").using("btree", table.eventType.asc().nullsLast()).where(sql`((event_type)::text <> 'NORMAL'::text)`),
	index("idx_classification_session_ts").using("btree", table.sessionId.asc().nullsLast(), table.ts.asc().nullsLast()),
check("classification_event_type_check", sql`((event_type)::text = ANY ((ARRAY['NORMAL'::character varying, 'NEAR_MISS'::character varying, 'FALL'::character varying])::text[]))`),check("classification_fall_type_check", sql`(((fall_type)::text = ANY ((ARRAY['FFH'::character varying, 'STF'::character varying])::text[])) OR (fall_type IS NULL))`),check("classification_responsiveness_check", sql`(((responsiveness)::text = ANY ((ARRAY['still'::character varying, 'active'::character varying])::text[])) OR (responsiveness IS NULL))`),]);

export const readingsInMonitoring = monitoring.table("readings", {
	readingId: bigserial("reading_id", { mode: 'number' }).primaryKey(),
	sessionId: integer("session_id").notNull().references(() => sessionsInMonitoring.sessionId, { onDelete: "cascade" } ),
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
	index("idx_readings_session_ts").using("btree", table.sessionId.asc().nullsLast(), table.ts.asc().nullsLast()),
]);

export const sessionsInMonitoring = monitoring.table("sessions", {
	sessionId: serial("session_id").primaryKey(),
	deviceId: text("device_id").notNull().references(() => identificationInDevices.deviceId, { onDelete: "restrict" } ),
	workerId: integer("worker_id").references(() => workersListInWorkers.workerId, { onDelete: "set null" } ),
	startTime: timestamp("start_time", { withTimezone: true }).default(sql`now()`).notNull(),
	endTime: timestamp("end_time", { withTimezone: true }),
	sampleRateHz: integer("sample_rate_hz").default(100).notNull(),
}, (table) => [
	index("idx_sessions_device").using("btree", table.deviceId.asc().nullsLast()),
	index("idx_sessions_open").using("btree", table.deviceId.asc().nullsLast()).where(sql`(end_time IS NULL)`),
	index("idx_sessions_worker").using("btree", table.workerId.asc().nullsLast()),
]);

export const nodeRegistrations = pgTable("node_registrations", {
	registrationId: bigserial("registration_id", { mode: 'number' }).primaryKey(),
	hardwareId: customType({ dataType: () => 'bytea' })("hardware_id").notNull(),
	nodeUuid: uuid("node_uuid").notNull(),
	active: boolean().default(true).notNull(),
	registeredAt: timestamp("registered_at", { withTimezone: true }).default(sql`now()`).notNull(),
	lastSeenAt: timestamp("last_seen_at", { withTimezone: true }).default(sql`now()`).notNull(),
	delinkedAt: timestamp("delinked_at", { withTimezone: true }),
}, (table) => [
	uniqueIndex("one_active_uuid_per_hardware").using("btree", table.hardwareId.asc().nullsLast()).where(sql`(active = true)`),
	unique("node_registrations_node_uuid_key").on(table.nodeUuid),check("node_registrations_hardware_id_check", sql`(octet_length(hardware_id) = 6)`),]);

export const workersListInWorkers = workers.table("workers_list", {
	workerId: serial("worker_id").primaryKey(),
	workerCode: varchar("worker_code", { length: 20 }).notNull(),
	siteAssigned: varchar("site_assigned", { length: 100 }),
	role: varchar({ length: 50 }),
	status: varchar({ length: 20 }).default("active").notNull(),
	createdAt: timestamp("created_at").default(sql`now()`).notNull(),
}, (table) => [
	unique("workers_list_worker_code_key").on(table.workerCode),check("workers_list_status_check", sql`((status)::text = ANY ((ARRAY['active'::character varying, 'inactive'::character varying])::text[]))`),]);
export const vEventsDetailInFallDetection = fallDetection.view("v_events_detail", {	classificationId: bigint("classification_id", { mode: 'number' }),
	ts: timestamp({ withTimezone: true }),
	eventType: varchar("event_type", { length: 20 }),
	fallType: varchar("fall_type", { length: 10 }),
	maxSmv: real("max_smv"),
	maxRotation: real("max_rotation"),
	responsiveness: varchar({ length: 10 }),
	activityBeforeEvent: varchar("activity_before_event", { length: 20 }),
	sessionId: integer("session_id"),
	deviceId: text("device_id"),
	bleAddress: text("ble_address"),
	workerCode: varchar("worker_code", { length: 20 }),
	siteAssigned: varchar("site_assigned", { length: 100 }),
	severity: varchar({ length: 10 }),
	acknowledged: boolean(),
}).as(sql`SELECT c.classification_id, c.ts, c.event_type, c.fall_type, c.max_smv, c.max_rotation, c.responsiveness, c.activity_before_event, s.session_id, di.device_id, di.ble_address, w.worker_code, w.site_assigned, a.severity, a.acknowledged FROM fall_detection.classification c JOIN monitoring.sessions s ON c.session_id = s.session_id JOIN devices.identification di ON s.device_id = di.device_id LEFT JOIN workers.workers_list w ON s.worker_id = w.worker_id LEFT JOIN fall_detection.alerts a ON a.classification_id = c.classification_id WHERE c.event_type::text <> 'NORMAL'::text ORDER BY c.ts DESC`);