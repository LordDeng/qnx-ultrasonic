#ifndef TIMING_H_
#define TIMING_H_

int timing_timespec_sub(struct timespec *result, struct timespec *x, struct timespec *y);
void timing_future_nanos(struct timespec *future, long my_nanos);

#endif
