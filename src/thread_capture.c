/**
 * @file    thread_capture.c
 * @author  AntCode42
 * @brief   CAPTURE stage: reads V4L2 frames and hands them to process_image().
 */

#include "synchronome.h"

/* single definitions of the capture-timing globals declared extern in
   synchronome.h */
int					framecnt = -8; /* always ignore the first 8 frames */
int					raw_pool_index = 0;
double				fnow = 0.0, fstart = 0.0, fstop = 0.0;
struct timespec		time_now, time_start, time_stop;
unsigned char		raw_pool[20][(HRES * VRES * 2)];
sem_t				raw_sem;

void errno_exit(const char *s)
{
	fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
	exit(EXIT_FAILURE);
}

int read_frame(void)
{
	struct	v4l2_buffer	buf;
	struct	timespec	t_start, t_end;
	double				elapsed_ms;

	CLEAR(buf);

	buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	buf.memory = V4L2_MEMORY_MMAP;

	/* measurement for WCET */
	clock_gettime(CLOCK_MONOTONIC, &t_start);

	if (-1 == xioctl(fd, VIDIOC_DQBUF, &buf))
	{
		switch (errno)
		{
			case EAGAIN:
				return 0;

			case EIO:
				/* could ignore EIO, but drivers should only set it for
				   serious errors, although some set it for non-fatal
				   errors too */
				return 0;

			default:
				printf("mmap failure\n");
				errno_exit("VIDIOC_DQBUF");
		}
	}

	/* measurement for WCET */
	clock_gettime(CLOCK_MONOTONIC, &t_end);
	elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
		   + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;
	syslog(LOG_INFO, "CAPTURE elapsed_ms=%.3f", elapsed_ms);

	assert(buf.index < n_buffers);

	sem_wait(&raw_sem);
	memcpy(raw_pool[raw_pool_index], buffers[buf.index].start, buf.bytesused);
	mq_send(q_capture_to_convert, (const char *)&raw_pool_index, sizeof(int), 0);

	raw_pool_index = (raw_pool_index + 1) % 20;

	if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
		errno_exit("VIDIOC_QBUF");

	return 1;
}

void mainloop(void)
{
	while (!abortProg)
	{
		fd_set			fds;
		struct timeval	tv;
		int				r;

		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		/* timeout */
		tv.tv_sec = 4;
		tv.tv_usec = 0;

		/* wait for a frame to be ready */
		r = select(fd + 1, &fds, NULL, NULL, &tv);

		if (-1 == r)
		{
			if (EINTR == errno)
				/* retry */
				continue;
			errno_exit("select");
		}

		if (0 == r)
		{
			fprintf(stderr, "select timeout\n");
			abortProg = TRUE;
			continue;
		}
		read_frame();
	}

	mq_send(q_capture_to_convert, (const char *)&sentinel, sizeof(int), 0);

	clock_gettime(CLOCK_MONOTONIC, &time_stop);
	fstop = (double)time_stop.tv_sec + (double)time_stop.tv_nsec / 1000000000.0;
}
