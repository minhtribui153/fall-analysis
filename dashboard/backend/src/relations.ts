import { defineRelations } from "drizzle-orm";
import * as schema from "./schema";

export const relations = defineRelations(schema, (r) => ({
	activityLogInFallDetection: {
		sessionsInMonitoring: r.one.sessionsInMonitoring({
			from: [r.activityLogInFallDetection.sessionId, r.activityLogInFallDetection.workerId],
			to: [r.sessionsInMonitoring.sessionId, r.sessionsInMonitoring.workerId]
		}),
	},
	sessionsInMonitoring: {
		activityLogInFallDetections: r.many.activityLogInFallDetection(),
		classificationInFallDetections: r.many.classificationInFallDetection(),
		registrationAuditInIdentifications: r.many.registrationAuditInIdentification(),
		readingsInMonitorings: r.many.readingsInMonitoring(),
		nodeRegistrations: r.many.nodeRegistrations(),
	},
	workersInIdentification: {
		classificationInFallDetections: r.many.classificationInFallDetection({
			from: r.workersInIdentification.workerId.through(r.alertsInFallDetection.acknowledgedBy),
			to: r.classificationInFallDetection.classificationId.through(r.alertsInFallDetection.classificationId)
		}),
		registrationAuditInIdentifications: r.many.registrationAuditInIdentification(),
		devicesInIdentifications: r.many.devicesInIdentification(),
		nodeRegistrations: r.many.nodeRegistrations(),
	},
	classificationInFallDetection: {
		workersInIdentifications: r.many.workersInIdentification(),
		sessionsInMonitoring: r.one.sessionsInMonitoring({
			from: [r.classificationInFallDetection.sessionId, r.classificationInFallDetection.workerId],
			to: [r.sessionsInMonitoring.sessionId, r.sessionsInMonitoring.workerId]
		}),
	},
	registrationAuditInIdentification: {
		nodeRegistration: r.one.nodeRegistrations({
			from: r.registrationAuditInIdentification.registrationId,
			to: r.nodeRegistrations.registrationId
		}),
		sessionsInMonitoring: r.one.sessionsInMonitoring({
			from: r.registrationAuditInIdentification.sessionId,
			to: r.sessionsInMonitoring.sessionId
		}),
		workersInIdentification: r.one.workersInIdentification({
			from: r.registrationAuditInIdentification.workerId,
			to: r.workersInIdentification.workerId
		}),
	},
	nodeRegistrations: {
		registrationAuditInIdentifications: r.many.registrationAuditInIdentification(),
		sessionsInMonitoring: r.one.sessionsInMonitoring({
			from: r.nodeRegistrations.currentSessionId,
			to: r.sessionsInMonitoring.sessionId
		}),
		devicesInIdentification: r.one.devicesInIdentification({
			from: r.nodeRegistrations.deviceId,
			to: r.devicesInIdentification.deviceId
		}),
		workersInIdentification: r.one.workersInIdentification({
			from: r.nodeRegistrations.workerId,
			to: r.workersInIdentification.workerId
		}),
	},
	readingsInMonitoring: {
		sessionsInMonitoring: r.one.sessionsInMonitoring({
			from: [r.readingsInMonitoring.sessionId, r.readingsInMonitoring.deviceId],
			to: [r.sessionsInMonitoring.sessionId, r.sessionsInMonitoring.deviceId]
		}),
	},
	devicesInIdentification: {
		workersInIdentifications: r.many.workersInIdentification({
			from: r.devicesInIdentification.deviceId.through(r.sessionsInMonitoring.deviceId),
			to: r.workersInIdentification.workerId.through(r.sessionsInMonitoring.workerId)
		}),
		nodeRegistrations: r.many.nodeRegistrations(),
	},
}))