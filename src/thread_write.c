/**
 * @file    thread_write.c
 * @author  AntCode42
 * @brief   WRITE stage: dumps a converted RGB frame to a PPM file.
 */

#include "synchronome.h"

char	ppm_header[] = "P6\n#9999999999 sec 9999999999 msec \n" HRES_STR " " VRES_STR "\n255\n";
char	ppm_dumpname[] = "frames/test0000.ppm";

void dump_ppm(const void *p, int size, unsigned int tag, struct timespec *time)
{
	int		written, total, dumpfd;
	struct	timespec t_start, t_end;
	double	elapsed_ms;

	/* measurement for WCET */
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	snprintf(&ppm_dumpname[11], 9, "%04d", tag);
	strncat(&ppm_dumpname[15], ".ppm", 5);
	dumpfd = open(ppm_dumpname, O_WRONLY | O_NONBLOCK | O_CREAT, 00666);

	snprintf(&ppm_header[4], 11, "%010d", (int)time->tv_sec);
	strncat(&ppm_header[14], " sec ", 5);
	snprintf(&ppm_header[19], 11, "%010d", (int)((time->tv_nsec) / 1000000));
	strncat(&ppm_header[29], " msec \n" HRES_STR " " VRES_STR "\n255\n", 19);

	/* subtract 1 from sizeof because it includes the string's null terminator */
	written = write(dumpfd, ppm_header, sizeof(ppm_header) - 1);

	total = 0;
	do
	{
		written = write(dumpfd, p, size);
		total += written;
	} while (total < size);

	/* measurement for WCET */
	clock_gettime(CLOCK_MONOTONIC, &t_end);
	elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
		   + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;
	syslog(LOG_INFO, "WRITE elapsed_ms=%.3f", elapsed_ms);

	clock_gettime(CLOCK_MONOTONIC, &time_now);
	fnow = (double)time_now.tv_sec + (double)time_now.tv_nsec / 1000000000.0;
	printf("Frame written to flash at %lf, %d, bytes\n", (fnow - fstart), total);

	close(dumpfd);
}

void writeloop(void)
{
	int				write_index;
	int				nbr_write;
	struct timespec	frame_time;

	nbr_write = 0;
	while (!abortProg)
	{
		mq_receive(q_diff_to_write, (char *)&write_index, sizeof(int), 0);
		syslog(LOG_INFO, "WRITE_RECEIVED index=%d", write_index);
		if (write_index < 0)
			break;
		if (FRAMES_TO_WRITE > nbr_write)
		{
			clock_gettime(CLOCK_REALTIME, &frame_time);
			dump_ppm(frame_pool[write_index], (((HRES * VRES * 2) * 6) / 4), nbr_write, &frame_time);
			sem_post(&frame_sem);
			nbr_write++;
		}
		else
		{
			abortProg = TRUE;
			break;
		}
	}
}
