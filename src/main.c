/**
 * @file    main.c
 * @author  AntCode42
 * @brief   Entry point: sets up threads, CPU affinity, V4L2, and the sequencer.
 */

#include "synchronome.h"

mqd_t	q_capture_to_convert;
mqd_t	q_convert_to_diff;
mqd_t	q_diff_to_write;

int		sentinel = -1;

int main(void)
{
	int					i, rc, scope, flags = 0;

	cpu_set_t			threadcpu;
	cpu_set_t			allcpuset;

	pthread_t			threads[NUM_THREADS];
	threadParams_t		threadParams[NUM_THREADS];
	pthread_attr_t		rt_sched_attr[NUM_THREADS];
	int					rt_max_prio, rt_min_prio, cpuidx;

	struct sched_param	rt_param[NUM_THREADS];
	struct sched_param	main_param;
	struct mq_attr		queue_attr;

	pthread_attr_t	main_attr;
	pid_t			mainpid;

	printf("Starting High Rate Sequencer Demo\n");
	clock_gettime(MY_CLOCK_TYPE, &start_time_val); start_realtime = realtime(&start_time_val);

	printf("System has %d processors configured and %d available.\n", get_nprocs_conf(), get_nprocs());

	CPU_ZERO(&allcpuset);

	for (i = 0; i < NUM_CPU_CORES; i++)
		CPU_SET(i, &allcpuset);

	printf("Using CPUS=%d from total available.\n", CPU_COUNT(&allcpuset));

	/* semaphore initialization */
	// if (sem_init(&semS1, 0, 0)) { printf("Failed to initialize S1 semaphore\n"); exit(-1); }     TO DELET
	if (sem_init(&raw_sem, 0, 20)) { printf("Failed to initialize raw_sem semaphore\n"); exit(-1); }
	if (sem_init(&frame_sem, 0, 20)) { printf("Failed to initialize frame_sem semaphore\n"); exit(-1); }

	mainpid = getpid();

	rt_max_prio = sched_get_priority_max(SCHED_FIFO);
	rt_min_prio = sched_get_priority_min(SCHED_FIFO);

	rc = sched_getparam(mainpid, &main_param);
	main_param.sched_priority = rt_max_prio;
	rc = sched_setscheduler(getpid(), SCHED_FIFO, &main_param);
	if (rc < 0) perror("main_param");
	print_scheduler();

	pthread_attr_getscope(&main_attr, &scope);

	if (scope == PTHREAD_SCOPE_SYSTEM)
		printf("PTHREAD SCOPE SYSTEM\n");
	else if (scope == PTHREAD_SCOPE_PROCESS)
		printf("PTHREAD SCOPE PROCESS\n");
	else
		printf("PTHREAD SCOPE UNKNOWN\n");

	printf("rt_max_prio=%d\n", rt_max_prio);
	printf("rt_min_prio=%d\n", rt_min_prio);

	for (i = 0; i < NUM_THREADS; i++)
	{
		/* Capture and Diff on core 2 */
		if (i == 1 || i == 3)
		{
			CPU_ZERO(&threadcpu);
			cpuidx = (1);
			CPU_SET(cpuidx, &threadcpu);
		}
		/* Convert on core 3 */
		else if (i == 2)
		{
			CPU_ZERO(&threadcpu);
			cpuidx = (2);
			CPU_SET(cpuidx, &threadcpu);
		}
		/* Write on core 4 */
		else
		{
			CPU_ZERO(&threadcpu);
			cpuidx = (3);
			CPU_SET(cpuidx, &threadcpu);
		}

		rc = pthread_attr_init(&rt_sched_attr[i]);
		rc = pthread_attr_setinheritsched(&rt_sched_attr[i], PTHREAD_EXPLICIT_SCHED);
		rc = pthread_attr_setschedpolicy(&rt_sched_attr[i], SCHED_FIFO);
		rc = pthread_attr_setaffinity_np(&rt_sched_attr[i], sizeof(cpu_set_t), &threadcpu);

		rt_param[i].sched_priority = rt_max_prio - i;
		pthread_attr_setschedparam(&rt_sched_attr[i], &rt_param[i]);

		threadParams[i].threadIdx = i;
	}

	printf("Service threads will run on %d CPU cores\n", CPU_COUNT(&threadcpu));

	/* V4L2 initialization */
	dev_name = "/dev/video0";
	open_device();
	init_device();
	start_capturing();

	/* message queue initialization */
	queue_attr.mq_flags = 0;
	queue_attr.mq_maxmsg = 10;
	queue_attr.mq_msgsize = sizeof(int);
	queue_attr.mq_curmsgs = 0;

	q_capture_to_convert = mq_open("/synchronome_capture_to_convert", O_CREAT | O_RDWR, 0666, &queue_attr);
	q_convert_to_diff = mq_open("/synchronome_convert_to_diff", O_CREAT | O_RDWR, 0666, &queue_attr);
	q_diff_to_write = mq_open("/synchronome_diff_to_write", O_CREAT | O_RDWR, 0666, &queue_attr);

	/* Create service threads, blocked awaiting release for: */

	/* Capture = RT_MAX-1 */
	rt_param[0].sched_priority = rt_max_prio - 1;
	pthread_attr_setschedparam(&rt_sched_attr[0], &rt_param[0]);
	rc = pthread_create(&threads[0], &rt_sched_attr[0], Capture, (void *)&(threadParams[0]));
	if (rc < 0)
		perror("pthread_create for Capture");
	else
		printf("pthread_create successful for Capture\n");

	/* Convert = RT_MAX-2 */
	rt_param[1].sched_priority = rt_max_prio - 2;
	pthread_attr_setschedparam(&rt_sched_attr[1], &rt_param[1]);
	rc = pthread_create(&threads[1], &rt_sched_attr[1], Convert, (void *)&(threadParams[1]));
	if (rc < 0)
		perror("pthread_create for Convert");
	else
		printf("pthread_create successful for Convert\n");

	/* Diff = RT_MAX-3 */
	rt_param[2].sched_priority = rt_max_prio - 3;
	pthread_attr_setschedparam(&rt_sched_attr[2], &rt_param[2]);
	rc = pthread_create(&threads[2], &rt_sched_attr[2], Diff, (void *)&(threadParams[2]));
	if (rc < 0)
		perror("pthread_create for Diff");
	else
		printf("pthread_create successful for Diff\n");

	/* Write = RT_MAX-4 */
	rt_param[3].sched_priority = rt_max_prio - 4;
	pthread_attr_setschedparam(&rt_sched_attr[3], &rt_param[3]);
	rc = pthread_create(&threads[3], &rt_sched_attr[3], Write, (void *)&(threadParams[3]));
	if (rc < 0)
		perror("pthread_create for Write");
	else
		printf("pthread_create successful for Write\n");

	/* arm the base-rate interval timer (currently unused, see synchronome.h note) */
	itime.it_interval.tv_sec = 0;
	itime.it_interval.tv_nsec = 10000000;
	itime.it_value.tv_sec = 0;
	itime.it_value.tv_nsec = 10000000;

	timer_settime(timer_1, flags, &itime, &last_itime);

	for (i = 0; i < NUM_THREADS; i++)
	{
		if (rc = pthread_join(threads[i], NULL) < 0)
			perror("main pthread_join");
		else
			printf("joined thread %d\n", i);
	}

	/* shutdown of the frame acquisition service */
	stop_capturing();

	printf("Total capture time=%lf, for %d frames, %lf FPS\n",
	       (fstop - fstart), FRAMES_TO_WRITE + 1, ((double)CAPTURE_FRAMES / (fstop - fstart)));

	uninit_device();
	close_device();
	fprintf(stderr, "\n");

	return 0;
}
