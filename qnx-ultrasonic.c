/*
 * Proj: 5
 * File: qnx-ultrasonic.c
 * Date: 15 November 2014
 * Auth: Steven Kroh (skk8768)
 *
 * Description:
 *
 * This file contains the main implementation of Lab 5. It obtains data
 * from the ultrasonic sensor every 1/10 second and displays the current
 * distance in inches to the terminal in a pretty manner.
 *
 * NOTE: The output of this program must be viewed on the QNX root console
 * over VGA. The display requirements of this lab may only be satisfied in that
 * environment, as only the QNX root console provides *all* termios
 * functionality. The eclipse console (grabbing text over qconn) does not
 * display carriage returns correctly (and thus will not prevent scrolling).
 */

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
#include <time.h>
#include <hw/inout.h>
#include <stdint.h> /* for uintptr_t */
#include <sys/neutrino.h> /* for ThreadCtl() */
#include <inttypes.h>
#include <sys/mman.h> /* for mmap_device_io() */
#include "timing.h"

/* Function prototypes */

static int get_micros_stub(void);
static int get_micros_ultrasonic(void);

static void prod(int(*get_micros)(void)); /* Producer thread */
static void cons(void); /* Consumer thread */
static void disp(void); /* Display thread */
static void qthd(void); /* Wait-for-quit thread */

/*
 * Converts a travel time in microseconds to a travel distance in inches
 */
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

static unsigned int min_inches = 0xFFFFFFFF; /* UINT_MAX sentinel value */
static unsigned int max_inches = 0x00000000; /* UINT_MIN sentinel value */

/*
 * This implementation of raw() is from the QNX documentation at the following
 * URL. The URL is split across multiple lines to fit within 80 chars. These
 * splits are denoted by backwards slashes.
 *
 * http://www.qnx.com/developers/docs/660/index.jsp?topic=\
 * %2Fcom.qnx.doc.neutrino.lib_ref%2Ftopic%2Ft%2Ftcsetattr.html\
 * &resultof=%22tcsetattr%22%20
 */
static int raw(int fd) {
	struct termios termios_p;

	if (tcgetattr(fd, &termios_p))
		return (-1);

	termios_p.c_cc[VMIN] = 1;
	termios_p.c_cc[VTIME] = 0;
	termios_p.c_lflag &= ~(ECHO | ICANON | ISIG | ECHOE | ECHOK | ECHONL);
	termios_p.c_oflag &= ~(OPOST);
	return (tcsetattr(fd, TCSADRAIN, &termios_p));
}

/**
 * Void helper function on STDIN_FILENO that can be used by atexit()
 */
static void raw_stdin() {
	raw(0);
}

/*
 * This implementation of unraw() is from the QNX documentation at the following
 * URL. The URL is split across multiple lines to fit within 80 chars. These
 * splits are denoted by backwards slashes.
 *
 * http://www.qnx.com/developers/docs/660/index.jsp?topic=\
 * %2Fcom.qnx.doc.neutrino.lib_ref%2Ftopic%2Ft%2Ftcsetattr.html\
 * &resultof=%22tcsetattr%22%20
 */
static int unraw(int fd) {
	struct termios termios_p;

	if (tcgetattr(fd, &termios_p))
		return (-1);

	termios_p.c_lflag |= (ECHO | ICANON | ISIG | ECHOE | ECHOK | ECHONL);
	termios_p.c_oflag |= (OPOST);
	return (tcsetattr(fd, TCSADRAIN, &termios_p));
}

/**
 * Void helper function on STDIN_FILENO that can be used by atexit()
 */
static void unraw_stdin() {
	unraw(0);
}

/*
 * Creates all threads and creates the message queues. Then, joins on all
 * threads and ends the program.
 *
 * If USE_STUB = 1, then a stub method will be used for micros input.
 * Otherwise, the ultrasonic sensor will be used for input.
 */
#define USE_STUB 0
int main(int argc, char *argv[]) {
	raw_stdin(); /* Use raw mode so getchar() doesn't buffer */
	atexit(&unraw_stdin); /* At the end of the program, unraw stdin */

	printf("Press any key to start measurements:\n\r");
	printf("To end the program, press 'q' or 'Q'\n\r");
	getchar();

	quit = 0; /* qthd sets this to 1 to end the program */

	/*
	 * To use nanosleep or clock_gettime effectively, we need to set the
	 * resolution of the realtime clock in the microsecond range.
	 */
	struct _clockperiod clk;
	clk.fract = 0;
	clk.nsec = 10000;
	ClockPeriod(CLOCK_REALTIME, &clk, NULL, 0);

	//	/*
	//	 * Use the code below to print out the realtime clock's resolution.
	//	 */
	//	struct timespec res;
	//	clock_getres(CLOCK_REALTIME, &res);
	//	printf("res: %ld", res.tv_nsec);

	/* Destroy the queues if they already exist (to refresh their params) */
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

	/* The producer thread has the highest priority to use hw */
	sched.sched_priority++;
	pthread_attr_setschedparam(&at_p, &sched);

	sched.sched_priority--;
	pthread_attr_setschedparam(&at_c, &sched);

	sched.sched_priority--;
	pthread_attr_setschedparam(&at_d, &sched);

	sched.sched_priority--;
	pthread_attr_setschedparam(&at_q, &sched);

	/*
	 * The producer thread will get data from the function stored in get_micros
	 */
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

	printf("\r\nMinimum valid inches recorded: %d", min_inches);
	printf("\r\nMaximum valid inches recorded: %d", max_inches);

	printf("\r\n");

	mq_close(mq_c);
	mq_close(mq_d);

	mq_unlink(MQ_C_NAME);
	mq_unlink(MQ_D_NAME);

	return 0;
}

/*
 * The quit thread's backing function waits until the quit key is entered.
 * Then, it changes the quit flag to true. This thread relies on STDIN being
 * in raw mode.
 */
static void qthd(void) {
	char ch;
	while (1) {
		read(STDIN_FILENO, &ch, 1);
		if (ch == 'q' || ch == 'Q') {
			break;
		}
	}

	pthread_mutex_lock(&quit_mutex);
	quit = 1;
	pthread_mutex_unlock(&quit_mutex);
}

/*
 * The stub function starts at 1000 micros and increments that base by 10
 * each time it is accessed. Use this stub to test the code infrastructure
 * without using the ultrasonic hw.
 */
static int get_micros_last = 1000;
static int get_micros_stub(void) {
	return (get_micros_last += 10);
}

static uintptr_t ctrl_handle; /* Handle for daq control register */
static uintptr_t dioa_handle; /* Handle for daq register A (out) */
static uintptr_t diob_handle; /* Handle for daq register B (in) */

# define BASE_ADDRESS 0x280
# define CTRL_ADDRESS (BASE_ADDRESS + 11)
# define DIOA_ADDRESS (BASE_ADDRESS + 8)
# define DIOB_ADDRESS (BASE_ADDRESS + 9)
# define PORT_LENGTH 1

# define CONTROL_REGISTER_CONFIG 0x2 /* Reg A out (trig); Reg B in (echo) */
# define HIGH 0xFF
# define LOW 0x00

/*
 * Triggers the ultrasonic sensor, waits for the echo, and measures the length
 * of the echo in microseconds. This function blocks until that measurement
 * is complete and returns the measurement in microseconds.
 */
static int get_micros_ultrasonic(void) {
	struct timespec init, post, elap;
	struct timespec pulse_time;
	pulse_time.tv_sec = (time_t) 0;
	pulse_time.tv_nsec = 10000; /* The initial trigger pulse is 10us */

	/* Pulse on A */
	out8(dioa_handle, HIGH );
	nanospin(&pulse_time);
	out8(dioa_handle, LOW );

	/* Wait for the sensor to send the pulse */
	pulse_time.tv_nsec = 4000;
	nanospin(&pulse_time);

	/* Taking the inverse of in8, we want to measure the low burst width.
	 * We want to see the burst width on any pin, so we have to use inequality
	 * operators on the whole incoming byte.
	 *
	 *          /- go low (~in8 == 0)
	 *          |
	 *          |     /- go high (~in8 > 0)
	 *          |     |
	 *          v     v
	 * +5 ``````|     |```````
	 *  0       |_____|
	 *
	 *          |<--->| low burst width is the length of the echo
	 *          t0    t
	 */
	int8_t signed_pulse;
	/* Poll B until high */
	while (1) {
		signed_pulse = ~in8(diob_handle);
		if (signed_pulse == (uint8_t) 0) {
			break;
		}
	}
	/* Start timer */
	clock_gettime(CLOCK_REALTIME, &init); /* Time t0 */
	/* Poll b until low */
	while (1) {
		signed_pulse = ~in8(diob_handle);
		if (signed_pulse > (int8_t) 0) {
			break;
		}
	}
	/* End timer */
	clock_gettime(CLOCK_REALTIME, &post); /* Time t */

	/* Calculate the elapsed time: t - t0 */
	timing_timespec_sub(&elap, &post, &init);

	/* Return microseconds */
	return (elap.tv_nsec / 1000);
}

/*
 * The producer thread obtains a raw data point from the get_micros function
 * every tenth of a second. This thread does not clean the data, it just passes
 * data points off to the consumer thread.
 *
 * This separation is designed such that the producer thread may run at the
 * highest priority with the effect of getting more time to handle the sensor.
 *
 * This thread communicates with the producer over the mq_c channel.
 */
#define QUEUE_PLUG -2
static void prod(int(*get_micros)(void)) {
	mqd_t mq_c = mq_open(MQ_C_NAME, O_WRONLY);

	/* Gain hw access permissions */
	ThreadCtl(_NTO_TCTL_IO, NULL);

	/* mmap data regions to handles */
	ctrl_handle = mmap_device_io(PORT_LENGTH, CTRL_ADDRESS);
	dioa_handle = mmap_device_io(PORT_LENGTH, DIOA_ADDRESS);
	diob_handle = mmap_device_io(PORT_LENGTH, DIOB_ADDRESS);

	/* Set control register bits */
	out8(ctrl_handle, CONTROL_REGISTER_CONFIG );

	struct timespec init, post, elap;
	struct timespec diff;
	diff.tv_sec = (time_t) 0;
	diff.tv_nsec = 100000000;

	int neg;
	int micros;
	char msg[sizeof(micros)];

	int thd_quit = 0;
	while (!thd_quit) {
		clock_gettime(CLOCK_REALTIME, &init); /* Time how long daq takes */

		pthread_mutex_lock(&quit_mutex);
		thd_quit = quit;
		pthread_mutex_unlock(&quit_mutex);

		micros = get_micros();
		memcpy(msg, &micros, sizeof(micros));

		mq_send(mq_c, msg, sizeof(micros), 0);

		clock_gettime(CLOCK_REALTIME, &post); /* End daq timer */
		/*
		 * Sleep the difference between the full 1/10 second and the
		 * elapsed daq time. (This creates a 1/10 second heartbeat
		 */
		neg = timing_timespec_sub(&elap, &post, &init);
		neg = timing_timespec_sub(&elap, &diff, &elap);
		/* If there is time before the next tenth-second, sleep */
		if (!neg) {
			clock_nanosleep(CLOCK_REALTIME, 0, &elap, NULL);
		}
	}

	/* Before ending the thread, plug the consumer's queue */
	micros = QUEUE_PLUG;
	memcpy(msg, &micros, sizeof(micros));
	mq_send(mq_c, msg, sizeof(micros), 0);
}

//static const int ULTRA_EXC_HIBND = 2147483647;
static const int ULTRA_EXC_HIBND = 20; /* The maximum permissible inch range */
static const int ULTRA_INC_LOBND = 0; /* The minimum permissible inch range */
/* Represents a number of inches not in the above range */
static const int ULTRA_INVALID = -1;
/*
 * The consumer thread receives microsecond messages from the producer,
 * converts those microseconds to inches, and forwards those results for
 * display by the display thread.
 */
static void cons() {
	mqd_t mq_c = mq_open(MQ_C_NAME, O_RDONLY);
	mqd_t mq_d = mq_open(MQ_D_NAME, O_WRONLY);

	int micros;
	char msg[sizeof(micros)];
	int inches;

	int thd_quit = 0;
	while (!thd_quit) {
		pthread_mutex_lock(&quit_mutex);
		thd_quit = quit;
		pthread_mutex_unlock(&quit_mutex);

		mq_receive(mq_c, msg, sizeof(micros), NULL);
		memcpy(&micros, msg, sizeof(micros));

		/* If no more data will be coming from the producer, quit now */
		if (micros == QUEUE_PLUG) {
			break;
		}

		inches = micros_to_inches(micros);
		if (ULTRA_INC_LOBND <= inches && inches < ULTRA_EXC_HIBND) {
			if (inches < min_inches) /* Calculate statistics */
				min_inches = inches;
			if (inches > max_inches)
				max_inches = inches;
		} else {
			inches = ULTRA_INVALID;
		}

		memcpy(msg, &inches, sizeof(inches));
		mq_send(mq_d, msg, sizeof(inches), 0);
	}

	/* Before ending the thread, plug the display thread's queue */
	inches = QUEUE_PLUG;
	memcpy(msg, &inches, sizeof(inches));
	mq_send(mq_d, msg, sizeof(inches), 0);
}

/*
 * Converts travel time in microseconds to travel distance in inches
 */
static const int IN_DIVISOR = 71;
static int micros_to_inches(int micros) {
	return micros / IN_DIVISOR / 2;
}

/*
 * The display thread receives inch messages from the consumer thread and
 * prints those messages out to the terminal.
 *
 * If a valid number of inches is received, that number is printed immediately
 * to the terminal.
 *
 * If an invalid number of inches is received, then this function will begin
 * to print a flashing asterisk with a period of ASTER_FLASH_PERIOD_NANOS.
 * Because this function uses the blocking mq_recieve family of functions to
 * receive data from the consumer thread, it must timeout when the asterisk
 * is to be flashed.
 */
static const int ASTER_FLASH_PERIOD_NANOS = 1000000000; /* 1 second */
static const size_t MAX_BUF = 80;
static void disp() {
	mqd_t mq_d = mq_open(MQ_D_NAME, O_RDONLY);

	struct timespec abs; /* Absolute time at which to timeout of receive */

	int inches = ULTRA_INVALID;
	char msg[sizeof(inches)];

	char buf[MAX_BUF];
	int aster_on = 0; /* 0 => " ", 1 => "*" */

	const int HALF_PERIOD = ASTER_FLASH_PERIOD_NANOS / 2;
	struct timespec init, post, elap;
	init.tv_sec = ULONG_MAX;
	init.tv_nsec = 0;

	printf("\rMeasurement in inches:\n");

	int thd_quit = 0;
	ssize_t sz;
	while (!thd_quit) {
		pthread_mutex_lock(&quit_mutex);
		thd_quit = quit;
		pthread_mutex_unlock(&quit_mutex);

		/* Timeout HALF_PERIOD nanos from now */
		timing_future_nanos(&abs, HALF_PERIOD);
		sz = mq_timedreceive(mq_d, msg, sizeof(inches), NULL, &abs);
		if (sz > 0) {
			memcpy(&inches, msg, sizeof(inches));
			if (inches == QUEUE_PLUG) {
				break; /* Quit if no more data is to be received */
			} else if (inches != ULTRA_INVALID) {
				/* Save the valid inches to the display buffer */
				snprintf(buf, MAX_BUF, "%d", inches);
			}
		} else {
			//perror("");
		}

		/*
		 * If the number of inches is invalid, we have to perform asterisk
		 * flashing logic (until a number of valid inches is received
		 */
		if (inches == ULTRA_INVALID) {
			clock_gettime(CLOCK_REALTIME, &post); /* Current time */
			/* Calculate elapsed time since we last changed the asterisk */
			int neg = timing_timespec_sub(&elap, &post, &init);
			/* If the elapsed time is greater than the half-period, flip it */
			if (neg || elap.tv_nsec > HALF_PERIOD) {
				if ((aster_on = !aster_on)) {
					snprintf(buf, MAX_BUF, "%s", "*");
				} else {
					snprintf(buf, MAX_BUF, "%s", " ");
				}

				/* Record time the asterisk was last changed (i.e., now) */
				clock_gettime(CLOCK_REALTIME, &init);
			}
		}

		/* Print out the contents of the buffer every cycle */
		printf("\r%20s", "");
		printf("\r%s", buf);
		fflush(stdout);
	}
}
