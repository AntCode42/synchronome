/**
 * @file    seqgen.c
 * @author  AntCode42
 * @brief   Thread entry points for the synchronome pipeline stages.
 * @details Each stage thread loops on abortProg and its own blocking
 *          loop() function (mainloop/convertloop/diffloop/writeloop).
 *          Adapted from Sam Siewert's seqgen example code, extended to
 *          4 services (CAPTURE, CONVERT, DIFF, WRITE) for this project.
 */

#include "synchronome.h"

void *Capture(void *threadp)
{
	struct	timespec	current_time_val;
	double				current_realtime;
	unsigned long long	S1Cnt = 0;

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Capture thread @ sec=%6.9lf\n", current_realtime - start_realtime);
	printf("Capture thread @ sec=%6.9lf\n", current_realtime - start_realtime);

	while (!abortProg)
		mainloop();

	/* on order of up to milliseconds of latency to get time */
	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Capture release %llu on core %d @ sec=%6.9lf\n", S1Cnt, sched_getcpu(), current_realtime - start_realtime);

	pthread_exit((void *)0);
}

void *Convert(void *threadp)
{
	struct	timespec	current_time_val;
	double				current_realtime;
	unsigned long long	S2Cnt = 0;

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Convert thread @ sec=%6.9lf\n", current_realtime - start_realtime);
	printf("Convert thread @ sec=%6.9lf\n", current_realtime - start_realtime);

	while (!abortProg)
		convertloop();

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Convert release %llu on core %d @ sec=%6.9lf\n", S2Cnt, sched_getcpu(), current_realtime - start_realtime);

	pthread_exit((void *)0);
}

void *Diff(void *threadp)
{
	struct	timespec	current_time_val;
	double				current_realtime;
	unsigned long long	S3Cnt = 0;

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Diff thread @ sec=%6.9lf\n", current_realtime - start_realtime);
	printf("Diff thread @ sec=%6.9lf\n", current_realtime - start_realtime);

	while (!abortProg)
		diffloop();

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Diff release %llu on core %d @ sec=%6.9lf\n", S3Cnt, sched_getcpu(), current_realtime - start_realtime);

	pthread_exit((void *)0);
}

void *Write(void *threadp)
{
	struct	timespec	current_time_val;
	double				current_realtime;
	unsigned long long	S4Cnt = 0;

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Write thread @ sec=%6.9lf\n", current_realtime - start_realtime);
	printf("Write thread @ sec=%6.9lf\n", current_realtime - start_realtime);

	while (!abortProg)
		writeloop();

	clock_gettime(MY_CLOCK_TYPE, &current_time_val); current_realtime = realtime(&current_time_val);
	syslog(LOG_CRIT, "Write release %llu on core %d @ sec=%6.9lf\n", S4Cnt, sched_getcpu(), current_realtime - start_realtime);

	pthread_exit((void *)0);
}

double realtime(struct timespec *tsptr)
{
	return ((double)(tsptr->tv_sec) + (((double)tsptr->tv_nsec) / 1000000000.0));
}

void print_scheduler(void)
{
	int schedType;

	schedType = sched_getscheduler(getpid());

	switch (schedType)
	{
		case SCHED_FIFO:
			printf("Pthread Policy is SCHED_FIFO\n");
			break;
		case SCHED_OTHER:
			printf("Pthread Policy is SCHED_OTHER\n"); exit(-1);
			break;
		case SCHED_RR:
			printf("Pthread Policy is SCHED_RR\n"); exit(-1);
			break;
		default:
			printf("Pthread Policy is UNKNOWN\n"); exit(-1);
	}
}
