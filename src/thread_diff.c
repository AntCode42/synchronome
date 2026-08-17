/**
 * @file    thread_diff.c
 * @author  AntCode42
 * @brief   Frame-differencing implementation for clock-tick detection.
 */

#include "synchronome.h"

/* Buffer holding the previous frame, used as the reference for pixel diffing */
unsigned char	prev_buffer[(HRES * VRES * 3)];
int				pool_index = 0;
static tick_state_t state = IDLE;   /* Unused at file scope (shadowed by the static in diff()) */


/**
 * @brief   Computes the percentage of change relative to background noise.
 * @details Maintains a sliding window of 30 "change" values considered
 *          background noise (no tick). The average of this window is
 *          used as the reference: if the current value exceeds it by,
 *          8% or more for 1 Hz goal and less than 5% or more for 10 Hz goal
 *			it's a tick, and the window is reset so the tick value
 *			doesn't pollute the next reference.
 */

 
int chg_diff(unsigned int change)
{
	static double  baseline = 0.0;       /* Running reference level (EMA of "no tick" changes) */
	static double  bootstrap_sum = 0.0;  /* Accumulator used to seed the baseline on startup */
	static int     bootstrap_cnt = 0;    /* Number of samples collected during bootstrap */
	int            prc;                  /* Change expressed as a percentage of baseline */

	prc = 0;

	/* Bootstrap phase: average the first 10 samples to get an initial baseline
	 * before attempting any real detection. */
	if (bootstrap_cnt < 10)
	{
    	bootstrap_sum += (double)change;
    	bootstrap_cnt++;
		fprintf(stderr, "BASELINE_SET baseline=%f sum=%f\n", baseline, bootstrap_sum);
        baseline = bootstrap_sum / 10.0;
    	return (0);   /* No reliable detection during bootstrap */
	}

	/* Express the current change as a percentage of the current baseline */
    prc = (int)(((double)change * 100.0) / baseline);
    syslog(LOG_INFO, "EMA_DEBUG baseline=%f prc=%d change=%u", baseline, prc, change);

    /* Only update the baseline (via exponential moving average) when the change
     * is still considered background noise (below the detection threshold),
     * so an actual tick doesn't pollute the reference level. */
    if (prc > DETN_SENS)
    	baseline = EMA_ALPHA * (double)change + (1.0 - EMA_ALPHA) * baseline;

    return (prc);
}

/**
 * @brief   Compares the current frame to the previous one (pixel diff).
 * @details On the first call, only initializes the reference buffer
 *          (no comparison possible without history). On subsequent
 *          calls, computes the sum of absolute differences, updates
 *          the reference buffer, then calls chg_diff() to decide
 *          whether this change is a tick. If so, logs the value via
 *          syslog.
 */
unsigned int diff(int diff_index)
{
 int                     i;
    static int              cnt;                       /* Number of frames processed so far (for start-up skip) */
    unsigned int            change;                     /* Sum of absolute pixel differences for this frame */
    struct timespec         t_start, t_end;              /* Used to time the STABLE_FOUND processing branch */
    double                  elapsed_ms;
    static struct timespec  last_tick_time;              /* Timestamp of the last detected tick (for debounce) */
    struct timespec         now;
    static tick_state_t     state = IDLE;                /* Tick detection state machine: IDLE -> TRANSITIONING -> STABLE_FOUND */
    static int              transition_count = 0;        /* Frames spent in TRANSITIONING (timeout guard) */
    static double           noise_floor = 0.0;           /* EMA of background (non-tick) change values */
    static int              noise_initialized = FALSE;   /* Whether noise_floor bootstrap is complete */
    static double           noise_sum = 0.0;             /* Accumulator for noise_floor bootstrap */
    static int              noise_cnt = 0;               /* Number of samples collected for noise_floor bootstrap */

    change = 0;

    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Warm-up: for the first START_UP_FRAMES frames, just seed prev_buffer
     * with the current frame. There is nothing to diff against yet. */
    if (cnt < START_UP_FRAMES)
    {
        cnt++;
        for (i = 0; i < (HRES * VRES * 3); i++)
            prev_buffer[i] = frame_pool[diff_index][i];
        sem_post(&frame_sem);
        return (change);
    }

    /* Sum of absolute per-byte differences between the current frame and
     * the previous one (naive frame-diff / motion metric). */
    for (i = 0; i < (HRES * VRES * 3); i++)
        change += abs(prev_buffer[i] - frame_pool[diff_index][i]);
    /* Update the reference buffer for the next call */
    for (i = 0; i < (HRES * VRES * 3); i++)
        prev_buffer[i] = frame_pool[diff_index][i];

    clock_gettime(CLOCK_MONOTONIC, &now);
    /* Time elapsed since the last confirmed tick, used to enforce a minimum
     * silence period (debounce) before a new tick can be detected. */
    elapsed_ms = (now.tv_sec - last_tick_time.tv_sec) * 1000.0
               + (now.tv_nsec - last_tick_time.tv_nsec) / 1e6;

    /* Bootstrap the noise floor over the first 10 post-warm-up frames,
     * the same way chg_diff() bootstraps its own baseline. */
    if (!noise_initialized)
    {
        noise_sum += (double)change;
        noise_cnt++;
        if (noise_cnt == 10)
        {
            noise_floor = noise_sum / 10.0;
            noise_initialized = TRUE;
        }
        sem_post(&frame_sem);
        return (change);
    }

    syslog(LOG_INFO, "PRC_DEBUG change=%u noise_floor=%f state=%d", change, noise_floor, state);

    /* --- Tick detection state machine ---
     * IDLE:          waiting for a change large enough (relative to the noise
     *                 floor) and far enough in time from the last tick to be
     *                 considered the start of a real event.
     * TRANSITIONING: a candidate event is in progress; wait for the change
     *                 to settle back down (below NOISE_MARGIN_LOW) before
     *                 confirming it, or force a confirmation after
     *                 MAX_TRANSITION_FRAMES to avoid getting stuck.
     * STABLE_FOUND:  the tick is confirmed; report it and reset to IDLE.
     */
    if (state == IDLE)
    {
        if ((double)change >= noise_floor * NOISE_MARGIN_HIGH && elapsed_ms >= SILENCE_MS)
        {
            /* Change spike detected outside the debounce window: start tracking
             * a potential tick instead of confirming it immediately, to avoid
             * reacting to a single noisy frame. */
            state = TRANSITIONING;
            transition_count = 0;
            last_tick_time = now;
        }
        else
        {
            /* Still background noise: keep adapting the noise floor via EMA */
            noise_floor = EMA_ALPHA * (double)change + (1.0 - EMA_ALPHA) * noise_floor;
        }
    }
    else if (state == TRANSITIONING)
    {
        transition_count++;
        if ((double)change < noise_floor * NOISE_MARGIN_LOW)
        {
            /* Change has settled back down: the event is confirmed as a tick */
            state = STABLE_FOUND;
        }
        else if (transition_count >= MAX_TRANSITION_FRAMES)
        {
            /* Safety net: don't stay stuck in TRANSITIONING forever if the
             * change never settles below the low margin. */
            state = STABLE_FOUND;
            syslog(LOG_WARNING, "TRANSITION_TIMEOUT forced STABLE_FOUND");
        }
    }

    if (state == STABLE_FOUND)
    {
        /* Confirmed tick: hand this frame index off to the writer thread/process
         * via the message queue, then reset the state machine. */
        syslog(LOG_INFO, "CHANGE %u", change);
        if (mq_send(q_diff_to_write, (const char *)&diff_index, sizeof(int), 0) == -1)
            syslog(LOG_ERR, "mq_send FAILED: %s", strerror(errno));
        state = IDLE;
        transition_count = 0;
        clock_gettime(CLOCK_MONOTONIC, &t_end);
        elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000.0
                   + (t_end.tv_nsec - t_start.tv_nsec) / 1e6;
        syslog(LOG_INFO, "DIFF elapsed_ms=%.3f", elapsed_ms);
    }
    else
    {
        /* No tick confirmed on this frame: release the frame buffer slot
         * back to the producer (frame_sem is NOT posted in the STABLE_FOUND
         * branch above -- see note below). */
        sem_post(&frame_sem);
    }

    return (change);
}

/**
 * @brief   Main loop for the diff thread/process.
 * @details Pulls frame indices from the convert->diff queue, runs diff()
 *          on each one, and forwards a sentinel value to the diff->write
 *          queue to signal shutdown when a negative index is received.
 */
void diffloop(void)
{
	int diff_index;

	while (1)
	{
		mq_receive(q_convert_to_diff, (char *)&diff_index, sizeof(int), 0);
		if (diff_index < 0)
		{
			/* Shutdown signal: forward the sentinel downstream and exit the loop */
			mq_send(q_diff_to_write, (const char *)&sentinel, sizeof(int), 0);
			break;
		}
		diff(diff_index);
	}
}
