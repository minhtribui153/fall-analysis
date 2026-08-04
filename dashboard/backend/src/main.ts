import 'dotenv/config';
import path from 'path';
import { NestFactory } from '@nestjs/core';
import { Pool } from 'pg';
import { AppModule } from './app.module';

async function runDatabaseQuery() {
  const pool = new Pool({
    connectionString: process.env.DATABASE_URL,
  });

  try {
    if (!process.env.DATABASE_URL) {
      throw new Error('DATABASE_URL is not set');
    }

    const result = await pool.query('SELECT * FROM fall_detection.alerts LIMIT 10');
    console.log('Fall detection alerts result:', result.rows);
  } catch (error) {
    console.error('Database query failed:', error instanceof Error ? error.message : error);
  } finally {
    await pool.end();
  }
}

async function bootstrap() {
  const app = await NestFactory.create(AppModule);
  await app.listen(3001);
  await runDatabaseQuery();
}

bootstrap().catch((error) => {
  console.error('Failed to start the application:', error instanceof Error ? error.message : error);
  process.exitCode = 1;
});

