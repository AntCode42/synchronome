/**
 * @file    synchronome.h
 * @author  AntCode42
 * @brief   Shared declarations for the synchronome pipeline.
 * @details Central header included by every .c file. Holds the single
 *          set of system includes, structs, constants, and extern/
 *          prototype declarations for globals defined once in their
 *          owning .c file (see comments below for ownership).
 *
 *
 *          Open structural note (not resolved here, design decision):
 *          - Sequencer-state globals (semS1, sequencePeriods, seqCnt)
 *            are defined in thread_convert.c and declared here, but are
 *            currently unused: the interval-timer-driven Sequencer()
 *            is stubbed out in seqgen.c and WRITE now runs as a
 *            blocking mq_receive() loop instead. Left in place as
 *            in-progress sequencer work rather than removed outright.
 */

#ifndef SYNCHRONOME_H
#define SYNCHRONOME_H

/* Must be defined before any system header is pulled in by this file
   or by any .c file that includes this header first. */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <mqueue.h>
#include <getopt.h>		/* getopt_long() */
#include <fcntl.h>		/* low-level i/o */
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/sysinfo.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <syslog.h>
#include <time.h>

/* ---- constants ---- */

#define CLEAR(x) memset(&(x), 0, sizeof(x))

#define HRES 640
#define VRES 480
#define HRES_STR "640"
#define VRES_STR "480"



#define START_UP_FRAMES (39)
#define LAST_FRAMES (1)
//#define CAPTURE_FRAMES (180 + LAST_FRAMES)
//#define FRAMES_TO_ACQUIRE (CAPTURE_FRAMES + START_UP_FRAMES + LAST_FRAMES)


#define FRAMES_PER_SEC (1)

#define USEC_PER_MSEC (1000)
#define NANOSEC_PER_MSEC (1000000)
#define NANOSEC_PER_SEC (1000000000)
#define NUM_CPU_CORES (4)
#define TRUE (1)
#define FALSE (0)

#define NUM_THREADS (4)

#define MY_CLOCK_TYPE CLOCK_MONOTONIC_RAW

#define EMA_ALPHA (0.1)


/* USAGE : for accquisition test at 10 Hz, please uncomment
   #define FREQ_10HZ. By default, the programme run at
   1 Hz. */
//#define FREQ_10HZ

#ifdef FREQ_10HZ
    #define SILENCE_MS 20
	#define NOISE_MARGIN_HIGH 1.07
	#define NOISE_MARGIN_LOW  1.04
    #define DETN_SENS 95
	#define LOW_DETN 40
	#define MAX_TRANSITION_FRAMES 15
	#define FRAMES_TO_WRITE (1800 + LAST_FRAMES)
	#define CAPTURE_FRAMES (1800 + LAST_FRAMES)
#else
	#define SILENCE_MS 400
	#define NOISE_MARGIN_HIGH 1.07
	#define NOISE_MARGIN_LOW  1.04
    #define DETN_SENS 108
	#define LOW_DETN 65
	#define MAX_TRANSITION_FRAMES 60
	#define FRAMES_TO_WRITE (180 + LAST_FRAMES)
	#define CAPTURE_FRAMES (180 + LAST_FRAMES)
#endif

/* ---- structs / typedefs ---- */

struct buffer
{
	void	*start;
	size_t	length;
};

typedef struct
{
	int threadIdx;
} threadParams_t;

typedef enum
{
    IDLE,
    TRANSITIONING,
    STABLE_FOUND
} tick_state_t;

/* ---- globals: V4L2 device state (defined in init.c) ---- */

extern char						*dev_name;
extern int						fd;
extern struct buffer			*buffers;
extern unsigned int				n_buffers;
extern int						force_format;
extern struct v4l2_format		fmt;

/* ---- globals: capture timing/state (defined in thread_capture.c) ---- */

extern int						framecnt;
extern double					fnow, fstart, fstop;
extern struct timespec			time_now, time_start, time_stop;
extern int						raw_pool_index;
extern sem_t					raw_sem;

/* ---- globals: sequencer state (defined in thread_convert.c - see note above) ---- */

extern int						abortProg;
// extern sem_t					semS1;   TO DELET
extern sem_t					frame_sem;
extern struct timespec			start_time_val;
extern double					start_realtime;
// extern unsigned long long		sequencePeriods;  TO DELET
extern timer_t					timer_1;
extern struct itimerspec		itime;
extern struct itimerspec		last_itime;
// extern unsigned long long		seqCnt;  TO DELET
extern int						sentinel;

/* ---- globals: message queues (defined in main.c) ---- */

extern mqd_t					q_capture_to_convert;
extern mqd_t					q_convert_to_diff;
extern mqd_t					q_diff_to_write;

/* ---- globals: frame storage pools ---- */

extern unsigned char			raw_pool[20][(HRES * VRES * 2)];
extern unsigned char			frame_pool[20][(HRES * VRES * 3)];

/* ---- globals: frame differencing / tick detection (defined in diff.c) ---- */

extern unsigned char			prev_buffer[(HRES * VRES * 3)];
extern int						pool_index;

/* ---- prototypes: cross-file utility functions ---- */

void	errno_exit(const char *s);
int		xioctl(int fh, int request, void *arg);

/* ---- prototypes: init.c / uninit.c (V4L2 lifecycle) ---- */

void	open_device(void);
void	init_device(void);
void	init_mmap(void);
void	start_capturing(void);
void	stop_capturing(void);
void	uninit_device(void);
void	close_device(void);

/* ---- prototypes: pipeline stages ---- */

int		read_frame(void);
void	mainloop(void);
void	process_image(const void *p, int size);
void	yuv2rgb(int y, int u, int v, unsigned char *r, unsigned char *g, unsigned char *b);
void	dump_ppm(const void *p, int size, unsigned int tag, struct timespec *time);

/**
 * @brief   Computes the percentage of change relative to background noise.
 * @param   change Sum of absolute pixel differences between 2 frames.
 * @return  Percentage (base 100) of change relative to the sliding-window
 *          average of the last recorded background-noise values.
 */
int				chg_diff(unsigned int change);

/**
 * @brief   Compares the current frame to the previous one and detects a tick.
 * @return  Sum of absolute pixel differences (0 for the very first frame,
 *          used only to initialize the reference buffer).
 */
unsigned int	diff(int diff_index);

/* ---- prototypes: seqgen.c ---- */

void	*Capture(void *threadp);
void	*Convert(void *threadp);
void	*Diff(void *threadp);
void	*Write(void *threadp);

double	realtime(struct timespec *tsptr);
void	print_scheduler(void);

void	convertloop(void);
void	writeloop(void);
void	diffloop(void);

#endif
