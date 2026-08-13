import 'dotenv/config';
import { NestFactory } from '@nestjs/core';
import { Pool } from 'pg';
import { AppModule } from './app.module';

async function bootstrap() {
  const app = await NestFactory.create(AppModule);
  app.enableCors({
    origin: ['http://localhost:3000', 'http://127.0.0.1:3000'],
    methods: 'GET,HEAD,PUT,PATCH,POST,DELETE,OPTIONS',
    credentials: true,
    allowedHeaders: ['Content-Type', 'Authorization'],
  });
  await app.listen(3001);
}

bootstrap().catch((error) => {
  console.error('Failed to start the application:', error instanceof Error ? error.message : error);
  process.exitCode = 1;
});

