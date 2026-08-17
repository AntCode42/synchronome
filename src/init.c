/**
 * @file    init.c
 * @author  AntCode42
 * @brief   V4L2 device initialization: open, capability/format negotiation.
 */

#include "synchronome.h"

/* single definitions of the V4L2 device-state globals declared extern
   in synchronome.h */
char				*dev_name;
int					fd = -1;
struct buffer		*buffers;
unsigned int		n_buffers;
int					force_format = 1;
struct v4l2_format	fmt;

void open_device(void)
{
	struct stat st;

	/* check the device exists and get its metadata */
	if (-1 == stat(dev_name, &st))
	{
		fprintf(stderr, "Cannot identify '%s': %d, %s\n",
			dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}

	/* a UVC webcam must appear as a character device */
	if (!S_ISCHR(st.st_mode))
	{
		fprintf(stderr, "%s is no device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	/* O_RDWR required: VIDIOC_S_FMT / VIDIOC_QBUF / VIDIOC_STREAMON ioctls
	   need FMODE_WRITE on the driver side, even with no direct write().
	   O_NONBLOCK: avoids a call blocking indefinitely; synchronization is
	   handled via select() in mainloop(). */
	fd = open(dev_name, O_RDWR | O_NONBLOCK, 0);

	if (-1 == fd)
	{
		fprintf(stderr, "Cannot open '%s': %d, %s\n",
			dev_name, errno, strerror(errno));
		exit(EXIT_FAILURE);
	}
}

void init_device(void)
{
	struct			v4l2_capability cap;
	struct			v4l2_cropcap cropcap;
	struct			v4l2_crop crop;
	unsigned int	min;

	/* query the driver's capabilities */
	if (-1 == xioctl(fd, VIDIOC_QUERYCAP, &cap))
	{
		/* EINVAL: this is not a V4L2 device (wrong driver) */
		if (EINVAL == errno)
		{
			fprintf(stderr, "%s is no V4L2 device\n", dev_name);
			exit(EXIT_FAILURE);
		}
		else
		{
			errno_exit("VIDIOC_QUERYCAP");
		}
	}

	/* can the device capture video? */
	if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
	{
		fprintf(stderr, "%s is no video capture device\n", dev_name);
		exit(EXIT_FAILURE);
	}

	/* streaming (MMAP) requires explicit driver support */
	if (!(cap.capabilities & V4L2_CAP_STREAMING))
	{
		fprintf(stderr, "%s does not support streaming i/o\n", dev_name);
		exit(EXIT_FAILURE);
	}

	/* best-effort attempt to reset cropping to the default rectangle:
	   any error is ignored, since many UVC webcams don't support cropping */
	CLEAR(cropcap);
	cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (0 == xioctl(fd, VIDIOC_CROPCAP, &cropcap))
	{
		crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		crop.c = cropcap.defrect; /* reset to default */

		if (-1 == xioctl(fd, VIDIOC_S_CROP, &crop))
		{
			/* cropping not supported, or any other error: ignored */
		}
	}

	/* format negotiation: resolution + pixel encoding */
	CLEAR(fmt);
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	if (force_format)
	{
		printf("FORCING FORMAT\n");
		fmt.fmt.pix.width  = HRES;
		fmt.fmt.pix.height = VRES;

		/* this one works for the Logitech C200 */
		fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
		fmt.fmt.pix.field = V4L2_FIELD_NONE; /* no interlacing */

		/* the driver may return a resolution different from the request */
		if (-1 == xioctl(fd, VIDIOC_S_FMT, &fmt))
			errno_exit("VIDIOC_S_FMT");
	}
	else
	{
		printf("ASSUMING FORMAT\n");
		/* preserve the format already set (e.g. via v4l2-ctl externally) */
		if (-1 == xioctl(fd, VIDIOC_G_FMT, &fmt))
			errno_exit("VIDIOC_G_FMT");
	}

	/* buggy driver paranoia: guard against inconsistent sizes */
	min = fmt.fmt.pix.width * 2;
	if (fmt.fmt.pix.bytesperline < min)
		fmt.fmt.pix.bytesperline = min;
	min = fmt.fmt.pix.bytesperline * fmt.fmt.pix.height;
	if (fmt.fmt.pix.sizeimage < min)
		fmt.fmt.pix.sizeimage = min;

	init_mmap();
}

void init_mmap(void)
{
	struct v4l2_requestbuffers req;

	CLEAR(req);

	req.count = 6;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	/* request buffer allocation for memory mapping */
	if (-1 == xioctl(fd, VIDIOC_REQBUFS, &req))
	{
		if (EINVAL == errno)
		{
			fprintf(stderr, "%s does not support memory mapping\n", dev_name);
			exit(EXIT_FAILURE);
		}
		else
		{
			errno_exit("VIDIOC_REQBUFS");
		}
	}

	/* check that at least 2 buffers were allocated */
	if (req.count < 2)
	{
		fprintf(stderr, "Insufficient buffer memory on %s\n", dev_name);
		exit(EXIT_FAILURE);
	}

	buffers = calloc(req.count, sizeof(*buffers));

	/* check for calloc failure (out of memory) */
	if (!buffers)
	{
		fprintf(stderr, "Out of memory\n");
		exit(EXIT_FAILURE);
	}

	/* configure each buffer: query its offset, then map it */
	for (n_buffers = 0; n_buffers < req.count; ++n_buffers)
	{
		struct v4l2_buffer buf;

		CLEAR(buf);

		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = n_buffers;

		if (-1 == xioctl(fd, VIDIOC_QUERYBUF, &buf))
			errno_exit("VIDIOC_QUERYBUF");

		buffers[n_buffers].length = buf.length;
		buffers[n_buffers].start =
			mmap(NULL /* let the kernel choose the address */,
			     buf.length,
			     PROT_READ | PROT_WRITE /* required by the V4L2 API */,
			     MAP_SHARED /* shared with the driver, not a private copy */,
			     fd, buf.m.offset);

		if (MAP_FAILED == buffers[n_buffers].start)
			errno_exit("mmap");
	}
}

void start_capturing(void)
{
	unsigned int i;
	enum v4l2_buf_type type;

	for (i = 0; i < n_buffers; ++i)
	{
		struct v4l2_buffer buf;

		printf("allocated buffer %d\n", i);

		CLEAR(buf);
		buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index = i;

		if (-1 == xioctl(fd, VIDIOC_QBUF, &buf))
			errno_exit("VIDIOC_QBUF");
	}

	type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (-1 == xioctl(fd, VIDIOC_STREAMON, &type))
		errno_exit("VIDIOC_STREAMON");
}
