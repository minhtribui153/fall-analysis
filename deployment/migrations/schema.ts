import {
  pgSchema,
  pgTable,
  bigserial,
  serial,
  text,
  integer,
  varchar,
  uuid,
  bigint,
  timestamp,
  real,
  boolean,
  index,
  uniqueIndex,
  unique,
  check,
} from "drizzle-orm/pg-core";
import { sql } from "drizzle-orm";

export const devices = pgSchema("devices");
export const fallDetection = pgSchema("fall_detection");
export const monitoring = pgSchema("monitoring");
export const workers = pgSchema("workers");

const deviceIdCheck = sql`device_id ~ '^([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}$'`;

export const identificationInDevices = devices.table(
  "identification",
  {
    deviceId: text("device_id").primaryKey(),
    nodeUuid: uuid("node_uuid").notNull(),
    bleAddress: text("ble_address"),
    registeredAt: timestamp("registered_at", { withTimezone: true })
      .defaultNow()
      .notNull(),
    lastSeenAt: timestamp("last_seen_at", { withTimezone: true })
      .defaultNow()
      .notNull(),
  },
  (table) => [
    unique("identification_node_uuid_key").on(table.nodeUuid),
    check("identification_device_id_format_check", deviceIdCheck),
  ],
);

export const nodeRegistrations = pgTable(
  "node_registrations",
  {
    registrationId: bigserial("registration_id", { mode: "number" }).primaryKey(),
    deviceId: text("device_id").notNull(),
    nodeUuid: uuid("node_uuid").notNull(),
    active: boolean("active").default(true).notNull(),
    registeredAt: timestamp("registered_at", { withTimezone: true })
      .defaultNow()
      .notNull(),
    lastSeenAt: timestamp("last_seen_at", { withTimezone: true })
      .defaultNow()
      .notNull(),
    delinkedAt: timestamp("delinked_at", { withTimezone: true }),
  },
  (table) => [
    unique("node_registrations_node_uuid_key").on(table.nodeUuid),
    uniqueIndex("one_active_registration_per_device")
      .on(table.deviceId)
      .where(sql`${table.active} = true`),
    index("idx_node_registrations_device_history").on(
      table.deviceId,
      table.registeredAt.desc(),
    ),
    check("node_registrations_device_id_format_check", deviceIdCheck),
    check(
      "node_registrations_delink_state_check",
      sql`(${table.active} = true AND ${table.delinkedAt} IS NULL) OR ${table.active} = false`,
    ),
  ],
);

export const workersListInWorkers = workers.table(
  "workers_list",
  {
    workerId: serial("worker_id").primaryKey(),
    workerCode: varchar("worker_code", { length: 20 }).notNull(),
    siteAssigned: varchar("site_assigned", { length: 100 }),
    role: varchar("role", { length: 50 }),
    status: varchar("status", { length: 20 }).default("active").notNull(),
    createdAt: timestamp("created_at", { withTimezone: true })
      .defaultNow()
      .notNull(),
  },
  (table) => [
    unique("workers_list_worker_code_key").on(table.workerCode),
    check(
      "workers_list_status_check",
      sql`${table.status} IN ('active', 'inactive')`,
    ),
  ],
);

export const sessionsInMonitoring = monitoring.table(
  "sessions",
  {
    sessionId: serial("session_id").primaryKey(),
    deviceId: text("device_id")
      .notNull()
      .references(() => identificationInDevices.deviceId, {
        onDelete: "restrict",
      }),
    workerId: integer("worker_id").references(
      () => workersListInWorkers.workerId,
      { onDelete: "set null" },
    ),
    startTime: timestamp("start_time", { withTimezone: true })
      .defaultNow()
      .notNull(),
    endTime: timestamp("end_time", { withTimezone: true }),
    sampleRateHz: integer("sample_rate_hz").default(100).notNull(),
  },
  (table) => [
    index("idx_sessions_device").on(table.deviceId),
    index("idx_sessions_open")
      .on(table.deviceId)
      .where(sql`${table.endTime} IS NULL`),
    index("idx_sessions_worker").on(table.workerId),
    check("sessions_sample_rate_check", sql`${table.sampleRateHz} > 0`),
    check(
      "sessions_time_order_check",
      sql`${table.endTime} IS NULL OR ${table.endTime} >= ${table.startTime}`,
    ),
  ],
);

export const readingsInMonitoring = monitoring.table(
  "readings",
  {
    readingId: bigserial("reading_id", { mode: "number" }).primaryKey(),
    sessionId: integer("session_id")
      .notNull()
      .references(() => sessionsInMonitoring.sessionId, {
        onDelete: "cascade",
      }),
    ts: timestamp("ts", { withTimezone: true }).notNull(),
    ax: real("ax").notNull(),
    ay: real("ay").notNull(),
    az: real("az").notNull(),
    gx: real("gx").default(0).notNull(),
    gy: real("gy").default(0).notNull(),
    gz: real("gz").default(0).notNull(),
    smv: real("smv"),
    gyroMagnitude: real("gyro_magnitude"),
  },
  (table) => [index("idx_readings_session_ts").on(table.sessionId, table.ts)],
);

export const classificationInFallDetection = fallDetection.table(
  "classification",
  {
    classificationId: bigserial("classification_id", {
      mode: "number",
    }).primaryKey(),
    sessionId: integer("session_id")
      .notNull()
      .references(() => sessionsInMonitoring.sessionId, {
        onDelete: "cascade",
      }),
    ts: timestamp("ts", { withTimezone: true }).notNull(),
    eventType: varchar("event_type", { length: 20 }).notNull(),
    fallType: varchar("fall_type", { length: 10 }),
    maxSmv: real("max_smv"),
    maxRotation: real("max_rotation"),
    freefallDuration: real("freefall_duration"),
    tiltChange: real("tilt_change"),
    responsiveness: varchar("responsiveness", { length: 10 }),
    activityBeforeEvent: varchar("activity_before_event", { length: 20 }),
    createdAt: timestamp("created_at", { withTimezone: true })
      .defaultNow()
      .notNull(),
  },
  (table) => [
    index("idx_classification_session_ts").on(table.sessionId, table.ts),
    index("idx_classification_events")
      .on(table.eventType)
      .where(sql`${table.eventType} <> 'NORMAL'`),
    check(
      "classification_event_type_check",
      sql`${table.eventType} IN ('NORMAL', 'NEAR_MISS', 'FALL')`,
    ),
    check(
      "classification_fall_type_check",
      sql`${table.fallType} IS NULL OR ${table.fallType} IN ('FFH', 'STF')`,
    ),
    check(
      "classification_responsiveness_check",
      sql`${table.responsiveness} IS NULL OR ${table.responsiveness} IN ('still', 'active')`,
    ),
  ],
);

export const activityLogInFallDetection = fallDetection.table(
  "activity_log",
  {
    activityId: bigserial("activity_id", { mode: "number" }).primaryKey(),
    sessionId: integer("session_id")
      .notNull()
      .references(() => sessionsInMonitoring.sessionId, {
        onDelete: "cascade",
      }),
    ts: timestamp("ts", { withTimezone: true }).notNull(),
    activityState: varchar("activity_state", { length: 20 }).notNull(),
  },
  (table) => [
    index("idx_activity_session_ts").on(table.sessionId, table.ts),
    check(
      "activity_log_activity_state_check",
      sql`${table.activityState} IN ('stationary', 'walking', 'running', 'jumping', 'unknown')`,
    ),
  ],
);

export const alertsInFallDetection = fallDetection.table(
  "alerts",
  {
    alertId: serial("alert_id").primaryKey(),
    classificationId: bigint("classification_id", { mode: "number" })
      .notNull()
      .references(() => classificationInFallDetection.classificationId, {
        onDelete: "cascade",
      }),
    raisedAt: timestamp("raised_at", { withTimezone: true })
      .defaultNow()
      .notNull(),
    severity: varchar("severity", { length: 10 }).default("high").notNull(),
    acknowledged: boolean("acknowledged").default(false).notNull(),
    acknowledgedBy: integer("acknowledged_by").references(
      () => workersListInWorkers.workerId,
      { onDelete: "set null" },
    ),
    acknowledgedAt: timestamp("acknowledged_at", { withTimezone: true }),
    notes: text("notes"),
  },
  (table) => [
    index("idx_alerts_unacknowledged")
      .on(table.raisedAt)
      .where(sql`${table.acknowledged} = false`),
    check(
      "alerts_severity_check",
      sql`${table.severity} IN ('low', 'medium', 'high')`,
    ),
    check(
      "alerts_acknowledgement_check",
      sql`(${table.acknowledged} = false AND ${table.acknowledgedAt} IS NULL) OR ${table.acknowledged} = true`,
    ),
  ],
);

export const vEventsDetailInFallDetection = fallDetection
  .view("v_events_detail", {
    classificationId: bigint("classification_id", { mode: "number" }),
    ts: timestamp("ts", { withTimezone: true }),
    eventType: varchar("event_type", { length: 20 }),
    fallType: varchar("fall_type", { length: 10 }),
    maxSmv: real("max_smv"),
    maxRotation: real("max_rotation"),
    responsiveness: varchar("responsiveness", { length: 10 }),
    activityBeforeEvent: varchar("activity_before_event", { length: 20 }),
    sessionId: integer("session_id"),
    deviceId: text("device_id"),
    bleAddress: text("ble_address"),
    workerCode: varchar("worker_code", { length: 20 }),
    siteAssigned: varchar("site_assigned", { length: 100 }),
    severity: varchar("severity", { length: 10 }),
    acknowledged: boolean("acknowledged"),
  })
  .as(sql`
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
    ORDER BY c.ts DESC
  `);
