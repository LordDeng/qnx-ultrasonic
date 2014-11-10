#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <fcntl.h>
#include <mqueue.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <termios.h>
#include "timing.h"

/* Function prototypes */

static int get_micros_stub(void);
static int get_micros_ultrasonic(void);

static void prod(int (*get_micros)(void));
static void cons(void);
static void disp(void);
static void qthd(void);

static int micros_to_inches(int micros);

/* 
 * Name of message queue to the consumer thread
 */
static const char* MQ_C_NAME = "/mq_to_cons";

/*
 * Name of message queue to the display thread
 */
static const char* MQ_D_NAME = "/mq_to_disp";

/*
 * Mutex over the quit boolean
 */
static pthread_mutex_t quit_mutex = PTHREAD_MUTEX_INITIALIZER;
static int quit;

/*
 * Send STDIN_FILENO into raw mode
 */
static void raw()
{
	struct termios tio;
	  
	tcgetattr(STDIN_FILENO, &tio);
	tio.c_lflag &= ~ICANON;
	tcsetattr(STDIN_FILENO, TCSANOW, &tio);
}

/*
 * Bring STDIN_FILENO out of raw mode
 */
static void noraw()
{
	struct termios tio;

	tcgetattr(STDIN_FILENO, &tio);
	tio.c_lflag |= ICANON;
	tcsetattr(STDIN_FILENO, TCSANOW, &tio);
}

/*
 * Creates all threads and creates the message queues. Then, joins on all
 * threads and ends the program.
 *
 * If USE_STUB = 1, then a stub method will be used for micros input.
 * Otherwise, the ultrasonic sensor will be used for input.
 */
#define USE_STUB 1
int main(int argc, char *argv[])
{
	raw(); /* Use raw mode so getchar() doesn't buffer */
	atexit(&noraw); /* At the end of the program, unraw stdin */

	printf("Press any key to start measurements:\n");
	getchar();

	quit = 0;

	/* Destroy the queues if they already exist (to refresh params) */
	mq_unlink(MQ_C_NAME);
	mq_unlink(MQ_D_NAME);

	struct mq_attr mq_at;
	mq_at.mq_flags = 0;
	mq_at.mq_maxmsg = 10;
	mq_at.mq_msgsize = sizeof(int); /* All messages are int-width */
	mq_at.mq_curmsgs = 0;

	mqd_t mq_c = mq_open(MQ_C_NAME, O_CREAT | O_RDONLY, S_IRWXU, &mq_at);
	mqd_t mq_d = mq_open(MQ_D_NAME, O_CREAT | O_RDONLY, S_IRWXU, &mq_at);

	pthread_attr_t at_p, at_c, at_d, at_q;
	pthread_attr_init(&at_p);
	pthread_attr_init(&at_c);
	pthread_attr_init(&at_d);
	pthread_attr_init(&at_q);

	int policy;
	struct sched_param sched;
	pthread_getschedparam(pthread_self(), &policy, &sched);

	sched.sched_priority--;
	pthread_attr_setschedparam(&at_q, &sched);

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

	pthread_t thd_qthd;
	pthread_create(&thd_qthd, &at_q, (void *) qthd, NULL);

	/* All threads do their work here */

	pthread_join(thd_qthd, NULL);
	pthread_join(thd_prod, NULL);
	pthread_join(thd_cons, NULL);
	pthread_join(thd_disp, NULL);

	printf("\rDone.\n");

	mq_close(mq_c);
	mq_close(mq_d);

	mq_unlink(MQ_C_NAME);
	mq_unlink(MQ_D_NAME);
}

/*
 * The quit thread's backing function waits until the quit key is entered.
 * Then, it changes the quit flag to true.
 */
static void qthd(void)
{
	char ch;
	while(1) {
		read(STDIN_FILENO, &ch, 1);
		if(ch == 'q' || ch == 'Q') {
			break;
		}
	}

	pthread_mutex_lock(&quit_mutex);
	quit = 1;
	pthread_mutex_unlock(&quit_mutex);
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

	int thd_quit = 0;
	while(!thd_quit) {
		clock_gettime(CLOCK_REALTIME, &init);

		pthread_mutex_lock(&quit_mutex);
		thd_quit = quit;
		pthread_mutex_unlock(&quit_mutex);

		micros = get_micros();
		memcpy(msg, &micros, sizeof(micros));

		mq_send(mq_c, msg, sizeof(micros), 0);

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

	struct timespec abs;

	int thd_quit = 0;
	while(!thd_quit) {
		pthread_mutex_lock(&quit_mutex);
		thd_quit = quit;
		pthread_mutex_unlock(&quit_mutex);

		timing_future_nanos(&abs, 500000000);
		mq_timedreceive(mq_c, msg, sizeof(micros), NULL, &abs);
		memcpy(&micros, msg, sizeof(micros));

		inches = micros_to_inches(micros);	
		if(ULTRA_INC_LOBND <= inches && inches < ULTRA_EXC_HIBND) {
		} else {
			inches = ULTRA_INVALID;
		}

		timing_future_nanos(&abs, 500000000);
		memcpy(msg, &inches, sizeof(inches));
		mq_timedsend(mq_d, msg, sizeof(inches), 0, &abs);
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
	struct timespec init, post, elap;
	init.tv_sec = ULONG_MAX;
	init.tv_nsec = 0;

	printf("\rMeasurement in inches:\n");

	int thd_quit = 0;
	ssize_t sz;
	while(!thd_quit) {
		pthread_mutex_lock(&quit_mutex);
		thd_quit = quit;
		pthread_mutex_unlock(&quit_mutex);

		timing_future_nanos(&abs, HALF_PERIOD);
		sz = mq_timedreceive(mq_d, msg, sizeof(inches), NULL, &abs);
		if(sz > 0) {
			memcpy(&inches, msg, sizeof(inches));
			if(inches != ULTRA_INVALID) {
				snprintf(buf, MAX_BUF, "%d", inches);
			}
		} else {
			//perror("");
		}

		if(inches == ULTRA_INVALID) {
			clock_gettime(CLOCK_REALTIME, &post);
			int neg = timing_timespec_sub(&elap, &post, &init);

			if(neg || elap.tv_nsec > HALF_PERIOD) {
				if((aster_on = !aster_on)) {
					snprintf(buf, MAX_BUF, "%s", "*");
				} else {
					snprintf(buf, MAX_BUF, "%s", " ");
				}

				clock_gettime(CLOCK_REALTIME, &init);
			}
		}

		printf("\r%80s", "");
		fflush(stdout);
		printf("\r%s", buf);
		fflush(stdout);
	}
}

