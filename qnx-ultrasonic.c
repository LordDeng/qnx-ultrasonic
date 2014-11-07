#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <mqueue.h>
#include <errno.h>
#include "timing.h"

static int get_micros_stub(void);
static int get_micros_ultrasonic(void);

static void prod(int (*get_micros)(void));
static void cons(void);
static void disp(void);

static int micros_to_inches(int micros);

static const char* MQ_C_NAME = "/mq_to_cons";
static const char* MQ_D_NAME = "/mq_to_disp";

#define DEBUGF printf

#define USE_STUB 1
int main(int argc, char *argv[])
{
	mq_unlink(MQ_C_NAME);
	mq_unlink(MQ_D_NAME);

	struct mq_attr mq_at;
	mq_at.mq_flags = 0;
	mq_at.mq_maxmsg = 10;
	mq_at.mq_msgsize = sizeof(int);
	mq_at.mq_curmsgs = 0;

	mqd_t mq_c = mq_open(MQ_C_NAME, O_CREAT | O_WRONLY, S_IRWXU, &mq_at);
	mqd_t mq_d = mq_open(MQ_D_NAME, O_CREAT | O_WRONLY, S_IRWXU, &mq_at);

	/*mq_getattr(mq_c, &mq_at);
	printf("long mq_maxmsg: %ld\n", mq_at.mq_maxmsg);
	printf("long mq_msgsize: %ld\n", mq_at.mq_msgsize);
	printf("long mq_curmsgs: %ld\n", mq_at.mq_curmsgs);*/

	pthread_attr_t at_p, at_c, at_d;
	pthread_attr_init(&at_p);
	pthread_attr_init(&at_c);
	pthread_attr_init(&at_d);

	int policy;
	struct sched_param sched;
	pthread_getschedparam(pthread_self(), &policy, &sched);

	sched.sched_priority--;
	pthread_attr_setschedparam(&at_p, &sched);

	sched.sched_priority--;
	pthread_attr_setschedparam(&at_c, &sched);

	sched.sched_priority--;
	pthread_attr_setschedparam(&at_d, &sched);

	int (*get_micros)(void);
#if USE_STUB
	get_micros = &get_micros_stub;
#else
	get_micros = &get_micros_ultrasonic;
#endif
	pthread_t thd_prod;
	pthread_create(&thd_prod, &at_p, (void *) prod, get_micros);

	pthread_t thd_cons;
	pthread_create(&thd_cons, &at_c, (void *) cons, NULL);

	pthread_t thd_disp;
	pthread_create(&thd_disp, &at_d, (void *) disp, NULL);

	pthread_join(thd_prod, NULL);
	pthread_join(thd_cons, NULL);
	pthread_join(thd_disp, NULL);

	mq_close(mq_c);
	mq_close(mq_d);

	mq_unlink(MQ_C_NAME);
	mq_unlink(MQ_D_NAME);
}

static int get_micros_last = 1000;
static int get_micros_stub(void)
{
	return (get_micros_last += 10);
}

static int get_micros_ultrasonic(void)
{
	/* Create real implementation later */
	return 0;
}

static void prod(int (*get_micros)(void))
{
	mqd_t mq_c = mq_open(MQ_C_NAME, O_WRONLY);

	struct timespec init, post, elap;
	struct timespec diff;
	diff.tv_sec = (time_t )0;
	diff.tv_nsec = 100000000;

	int neg;
	int micros;
	char msg[sizeof(micros)];

	while(1) {
		clock_gettime(CLOCK_REALTIME, &init);

		micros = get_micros();
		memcpy(msg, &micros, sizeof(micros));

		mq_send(mq_c, msg, sizeof(micros), 0);
		//DEBUGF("prod->cons\n");

		clock_gettime(CLOCK_REALTIME, &post);
		neg = timing_timespec_sub(&elap, &post, &init);
		neg = timing_timespec_sub(&elap, &diff, &elap);

		/* If there is time before the next tenth-second, sleep */
		if(!neg) {
			clock_nanosleep(CLOCK_REALTIME, 0, &elap, NULL);	
		}
	}
}

static const int ULTRA_EXC_HIBND = 30;
static const int ULTRA_INC_LOBND = 0;
static const int ULTRA_INVALID = -1;
static void cons()
{
	mqd_t mq_c = mq_open(MQ_C_NAME, O_RDONLY);
	mqd_t mq_d = mq_open(MQ_D_NAME, O_WRONLY);

	int micros;
	char msg[sizeof(micros)];

	int inches;

	while(1) {
		mq_receive(mq_c, msg, sizeof(micros), NULL);
		memcpy(&micros, msg, sizeof(micros));

		inches = micros_to_inches(micros);	
		if(ULTRA_INC_LOBND <= inches && inches < ULTRA_EXC_HIBND) {
		} else {
			inches = ULTRA_INVALID;
		}

		memcpy(msg, &inches, sizeof(inches));
		mq_send(mq_d, msg, sizeof(inches), 0);
	}
}

static const int IN_DIVISOR = 71;
static int micros_to_inches(int micros)
{
	return micros / IN_DIVISOR / 2;
}

static const int ASTER_FLASH_PERIOD_NANOS = 1000000000; /* 1 second */
static const size_t MAX_BUF = 80;
static void disp()
{
	mqd_t mq_d = mq_open(MQ_D_NAME, O_RDONLY);

	struct timespec abs;

	int inches = ULTRA_INVALID;
	char msg[sizeof(inches)];

	char buf[MAX_BUF];
	int aster_on = 0;

	const int HALF_PERIOD = ASTER_FLASH_PERIOD_NANOS / 2;
	
	printf("Measurement in inches:\n");

	ssize_t sz;
	while(1) {
		timing_future_nanos(&abs, HALF_PERIOD);
		sz = mq_timedreceive(mq_d, msg, sizeof(inches), NULL, &abs);
		if(sz > 0) {
			//printf("hello\n");
			memcpy(&inches, msg, sizeof(inches));
			snprintf(buf, MAX_BUF, "%d  ", inches);
		} else {
			perror("");
		}

		if(inches == ULTRA_INVALID) {
			if((aster_on = !aster_on)) {
				snprintf(buf, MAX_BUF, "%s", "*  ");
			} else {
				snprintf(buf, MAX_BUF, "%s", "   ");
			}
		}

		printf("%s\r", buf);
		fflush(stdout);
	}
}

