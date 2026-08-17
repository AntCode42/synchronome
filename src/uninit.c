/**
 * @file    uninit.c
 * @author  AntCode42
 * @brief   V4L2 teardown: unmap buffers, stop streaming.
 */

#include "synchronome.h"

void uninit_device(void)
{
	unsigned int i;

	for (i = 0; i < n_buffers; ++i)
		if (-1 == munmap(buffers[i].start, buffers[i].length))
			errno_exit("munmap");

	free(buffers);
}

void stop_capturing(void)
{
	enum v4l2_buf_type type;

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_STREAMOFF, &type))
		errno_exit("VIDIOC_STREAMOFF");
}

void close_device(void)
{
	if (-1 == close(fd))
		errno_exit("close");

	fd = -1;
}
