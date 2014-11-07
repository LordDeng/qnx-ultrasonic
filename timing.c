#include <unistd.h>
#include <time.h>
#include "timing.h"

/*
 * Proj: 5
 * File: timing.c
 * Date: 6 November 2014
 * Auth: Steven Kroh (skk8768)
 *
 * Description:
 *
 * This file contains the implementation of the public timing helper interface.
 * It provides functions for working with the obtuse timespec structures.
 */

/*
 * The following function is a modified version of the code provided in GNU's
 * documentation online:
 *
 * http://www.gnu.org/software/libc/manual/html_node/Elapsed-Time.html
 *
 * This function performs Result = X - Y on the provided timespecs. NOTE: the
 * input Y to this function is mutated as a result of the computation.
 *
 * Params: result - The difference
 *         x      - The minuend
 *         y      - The subtrahend
 * Return: 1      - If difference is negative
 *         0      - If difference is positive
 */
int timing_timespec_sub(result, x, y)
	struct timespec *result, *x, *y;
{
	/* Perform the carry for the later subtraction by updating y. */
	if (x->tv_nsec < y->tv_nsec) {
		int nums = (y->tv_nsec - x->tv_nsec) / 1000000000 + 1;
		y->tv_nsec -= 1000000000 * nums;
		y->tv_sec += nums;
	}
	if (x->tv_nsec - y->tv_nsec > 1000000000) {
		int nums = (x->tv_nsec - y->tv_nsec) / 1000000000;
		y->tv_nsec += 1000000000 * nums;
		y->tv_sec -= nums;
	}

	/* Compute the time remaining to wait.
	 tv_usec is certainly positive. */
	result->tv_sec = x->tv_sec - y->tv_sec;
	result->tv_nsec = x->tv_nsec - y->tv_nsec;

	/* Return 1 if result is negative. */
	return x->tv_sec < y->tv_sec;
}

void timing_future_nanos(struct timespec *future, long my_nanos)
{
	clock_gettime(CLOCK_REALTIME, future);
	long rt_nanos = future->tv_nsec;

	if(rt_nanos + my_nanos > 1000000000) {
		future->tv_nsec = (rt_nanos + my_nanos) - 1000000000;
		future->tv_sec++;
	} else {
		future->tv_nsec += my_nanos;
	} 
}

