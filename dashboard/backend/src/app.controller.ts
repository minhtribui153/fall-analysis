import { Controller, Get, Query, Res, Req } from '@nestjs/common';
import { Response, Request } from 'express';
import { AppService } from './app.service';

@Controller()
export class AppController {
  constructor(private readonly appService: AppService) {}

  @Get()
  getHello(): string {
    return this.appService.getHello();
  }

  @Get('v_events_detail')
  async getEventDetails() {
    return this.appService.getEventDetails();
  }

  @Get('stream-events')
  streamEventCounts(@Req() req: Request, @Res() res: Response) {
    res.setHeader('Content-Type', 'text/event-stream');
    res.setHeader('Cache-Control', 'no-cache');
    res.setHeader('Connection', 'keep-alive');
    res.setHeader('Access-Control-Allow-Origin', '*');

    const interval = setInterval(async () => {
      try {
        const counts = await this.appService.getEventCounts();
        res.write(`data: ${JSON.stringify(counts)}\n\n`);
      } catch (error) {
        console.error('Error streaming events:', error);
        clearInterval(interval);
        res.end();
      }
    }, 2000);

    req.on('close', () => {
      clearInterval(interval);
      res.end();
    });
  }

  @Get('activity-log')
  async getActivityLog(
    @Query('limit') limit?: string,
    @Query('offset') offset?: string,
  ) {
    return this.appService.getActivityLog(
      limit ? parseInt(limit, 10) : undefined,
      offset ? parseInt(offset, 10) : undefined,
    );
  }

  @Get('classification')
  async getClassification(
    @Query('limit') limit?: string,
    @Query('offset') offset?: string,
  ) {
    return this.appService.getClassification(
      limit ? parseInt(limit, 10) : undefined,
      offset ? parseInt(offset, 10) : undefined,
    );
  }
}
