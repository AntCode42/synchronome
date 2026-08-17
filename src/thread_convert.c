/**
 * @file    thread_convert.c
 * @author  AntCode42
 * @brief   CONVERT stage: YUYV -> RGB conversion, plus sequencer globals.
 * @details Sequencer-related globals (abortProg, semS1, timer state) are
 *          defined here for now, though they logically belong with
 *          seqgen.c - left as-is, see synchronome.h note. Several of
 *          them (semS1, sequencePeriods, seqCnt) are currently unused
 *          pending the Sequencer() rework.
 */

#include "synchronome.h"

/* single definitions of the sequencer-state globals declared extern in
   synchronome.h (see file-level note on ownership) */
int					abortProg = FALSE;
// sem_t				semS1;   TO DELET
sem_t				frame_sem;
struct timespec		start_time_val;
double				start_realtime;
// unsigned long long	sequencePeriods;   TO DELET
timer_t				timer_1;
struct itimerspec	itime = {{1, 0}, {1, 0}};
struct itimerspec	last_itime;
unsigned char		frame_pool[20][(HRES * VRES * 3)];
// unsigned long long	seqCnt = 0;  TO DELET

int xioctl(int fh, int request, void *arg)
{
	int r;

	do
	{
		r = ioctl(fh, request, arg);

	} while (-1 == r && EINTR == errno);

	return r;
}

/* conversion from camera YUYV to RGB */
void yuv2rgb(int y, int u, int v, unsigned char *r, unsigned char *g, unsigned char *b)
{
	int r1, g1, b1;

	/* replaces floating point coefficients */
	int c = y - 16, d = u - 128, e = v - 128;

	/* conversion that avoids floating point */
	r1 = (298 * c           + 409 * e + 128) >> 8;
	g1 = (298 * c - 100 * d - 208 * e + 128) >> 8;
	b1 = (298 * c + 516 * d           + 128) >> 8;

	/* computed values may need clipping */
	if (r1 > 255) r1 = 255;
	if (g1 > 255) g1 = 255;
	if (b1 > 255) b1 = 255;

	if (r1 < 0) r1 = 0;
	if (g1 < 0) g1 = 0;
	if (b1 < 0) b1 = 0;

	*r = r1;
	*g = g1;
	*b = b1;
}

void process_image(const void *p, int size)
{
	int				i, newi;
	int				y_temp, y2_temp, u_temp, v_temp;
	unsigned char	*pptr = (unsigned char *)p;
	struct timespec	t_start, t_end;
	double			elapsed_ms;

	sem_wait(&frame_sem);

	framecnt++;
	syslog(LOG_INFO, "frame %d: ", framecnt);

	if (framecnt == 0)
	{
		clock_gettime(CLOCK_MONOTONIC, &time_start);
		fstart = (double)time_start.tv_sec + (double)time_start.tv_nsec / 1000000000.0;
	}

	/* Pixels are YU and YV alternating, so YUYV which is 4 bytes.
	   We want RGB, so RGBRGB which is 6 bytes. */
	clock_gettime(CLOCK_MONOTONIC, &t_start);
	for (i = 0, newi = 0; i < size; i = i + 4, newi = newi + 6)
	{
		y_temp = (int)pptr[i]; u_temp = (int)pptr[i + 1];
		y2_temp = (int)pptr[i + 2]; v_temp = (int)pptr[i + 3];
		yuv2rgb(y_temp, u_temp, v_temp,
			&frame_pool[pool_index][newi],
			&frame_pool[pool_index][newi + 1],
			&frame_pool[pool_index][newi + 2]);
		yuv2rgb(y2_temp, u_temp, v_temp,
			&frame_pool[pool_index][newi + 3],
			&frame_pool[pool_index][newi + 4],
			&frame_pool[pool_index][newi + 5]);
	}
	mq_send(q_convert_to_diff, (const char *)&pool_index, sizeof(int), 0);

	clock_gettime(CLOCK_MONOTONIC, &t_end);
	elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
		   + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;
	syslog(LOG_INFO, "CONVERT elapsed_ms=%.3f", elapsed_ms);

	pool_index = (pool_index + 1) % 20;
}

void convertloop(void)
{
	int	frame_index;

	while (1)
	{
		mq_receive(q_capture_to_convert, (char *)&frame_index, sizeof(int), 0);
		if (frame_index < 0)
		{
			mq_send(q_convert_to_diff, (const char *)&sentinel, sizeof(int), 0);
			break;
		}
		process_image(raw_pool[frame_index], (HRES * VRES * 2));
		sem_post(&raw_sem);
	}
}
