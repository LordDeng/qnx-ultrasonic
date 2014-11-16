#ifndef TIMING_H_
#define TIMING_H_

/*
 * Proj: 5
 * File: timing.h
 * Date: 15 November 2014
 * Auth: Steven Kroh (skk8768)
 *
 * Description:
 *
 * This file contains the public interface to the timing helper module.
 * It specifies functions for working with the obtuse timespec structures.
 */

int timing_timespec_sub(struct timespec *result, struct timespec *x,
		struct timespec *y);
void timing_future_nanos(struct timespec *future, long my_nanos);

#endif
