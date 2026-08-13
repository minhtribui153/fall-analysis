import { Injectable } from '@nestjs/common';
import { Pool } from 'pg';

@Injectable()
export class AppService {
  private readonly pool = new Pool({
    connectionString: process.env.DATABASE_URL,
  });

  getHello(): string {
    return 'Hello World!';
  }

  async getEventDetails() {
    if (!process.env.DATABASE_URL) {
      throw new Error('DATABASE_URL is not set');
    }

    const result = await this.pool.query(`
      SELECT *
      FROM fall_detection.v_events_detail
      ORDER BY ts DESC;
    `);

    return result.rows;
  }

  async getEventCounts() {
    if (!process.env.DATABASE_URL) {
      throw new Error('DATABASE_URL is not set');
    }

    const result = await this.pool.query(`
      SELECT event_type, COUNT(*) as count
      FROM fall_detection.v_events_detail
      GROUP BY event_type;
    `);

    const counts = {
      fallCount: 0,
      nearMissCount: 0,
    };

    result.rows.forEach((row: any) => {
      if (row.event_type === 'FALL') {
        counts.fallCount = parseInt(row.count, 10);
      } else if (row.event_type === 'NEAR_MISS') {
        counts.nearMissCount = parseInt(row.count, 10);
      }
    });

    return counts;
  }

  async getActivityLog(limit?: number, offset?: number) {
    if (!process.env.DATABASE_URL) {
      throw new Error('DATABASE_URL is not set');
    }

    let query = `
      SELECT *
      FROM fall_detection.activity_log
      ORDER BY ts DESC
    `;

    if (limit) {
      query += ` LIMIT ${limit}`;
    }
    if (offset) {
      query += ` OFFSET ${offset}`;
    }
    query += ';';

    const result = await this.pool.query(query);
    return result.rows;
  }

  async getClassification(limit?: number, offset?: number) {
    if (!process.env.DATABASE_URL) {
      throw new Error('DATABASE_URL is not set');
    }

    let query = `
      SELECT *
      FROM fall_detection.classification
      ORDER BY ts DESC
    `;

    if (limit) {
      query += ` LIMIT ${limit}`;
    }
    if (offset) {
      query += ` OFFSET ${offset}`;
    }
    query += `;`;

    const result = await this.pool.query(query);
    return result.rows;
  }
}
