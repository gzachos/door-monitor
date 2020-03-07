/*
 * +-----------------------------------------------------------------------+
 * |             Copyright (C) 2017-2020 George Z. Zachos                  |
 * +-----------------------------------------------------------------------+
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Contact Information:
 * Name: George Z. Zachos
 * Email: gzzachos <at> gmail.com
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <wiringPi.h>

/* Global definitions */
#define __STR(val)                       #val
#define STR(val)                         __STR(val)
#define NUM_THREADS                      1  // Number of joinable threads including main
#define REED_SENSOR_PIN                  0
#define BUZZER_PIN                       13
#define ALERT_SWITCH_PIN                 2
#define MASTER_SWITCH_PIN                3
#define SWITCH_ISON(pin)                 (digitalRead(pin) == LOW)
#define DOOR_IS_CLOSED(state)            (state == LOW)
#define DOOR_WAS_OPEN(state)             (state == HIGH)
#define DOOR_WAS_CLOSED(state)           (state == LOW)
#define DOOR_CLOSED(state,prev_state) \
                (DOOR_WAS_OPEN(prev_state) && DOOR_IS_CLOSED(state))
#define DOOR_OPENED(state,prev_state) \
                (DOOR_WAS_CLOSED(prev_state) && !DOOR_IS_CLOSED(state))

/* Global definitions */
typedef struct timeval timeval_t;
typedef struct thrarg_s {
	timeval_t timeval;
	char      *state;
} thrarg_t;

enum thread_index {MAIN_THREAD};

/* Function prototypes */
long      get_curr_time(timeval_t *tv);
long      get_timestamp(timeval_t tv);
thrarg_t *alloc_arg(timeval_t timeval, char *state);
void     *notify_by_mail(void *arg);
void      request_termination(int signo);
void     *hit_buzzer(void *arg);
int       timevalcmp(timeval_t tv1, timeval_t tv2);

/* Global data */
volatile sig_atomic_t termination_requested = 0,  // RW access only by main thread
                      exit_signal = 0;            // RW access only by main thread
volatile int exitflag = 0;   // Shared between all joinable threads
volatile timeval_t latest_closed_door_tv; // Shared between all joinable threads
pthread_mutex_t exitflag_mutex = PTHREAD_MUTEX_INITIALIZER,
                closed_door_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t tids[NUM_THREADS];


int main(void)
{
	volatile int    curr_state,  // Door state
	                prev_state = -1;
	pthread_attr_t  attr;
	pthread_t       tid;
	thrarg_t       *arg, *buzzer_arg;
	timeval_t       timeval;
	long            timestamp;

	openlog("door-monitor", LOG_PID, LOG_USER);
	timestamp = get_curr_time(&timeval);
	syslog(LOG_INFO, "%ld: Door monitoring started", timestamp);

	tids[MAIN_THREAD] = pthread_self();

	signal(SIGTERM, &request_termination);
	signal(SIGINT,  &request_termination);
	signal(SIGUSR1, &request_termination);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	wiringPiSetup();
	if (piHiPri(99))
		syslog(LOG_INFO, "piHiPri: %s", strerror(errno));

	pinMode(REED_SENSOR_PIN, INPUT);
	pinMode(BUZZER_PIN, OUTPUT);
	pinMode(ALERT_SWITCH_PIN, INPUT);
	pinMode(MASTER_SWITCH_PIN, INPUT);

	/* Monitor door state (open/closed) until termination is requested by signal */
	for (; ;)
	{
		if (termination_requested)
		{
			if (pthread_mutex_trylock(&exitflag_mutex) == 0)
			{
				syslog(LOG_INFO, "Termination requested (%s)",
						strsignal(exit_signal));
				exitflag = termination_requested;
				pthread_mutex_unlock(&exitflag_mutex);
				break;
			}
		}

		curr_state = digitalRead(REED_SENSOR_PIN);
		timestamp = get_curr_time(&timeval);

		if (DOOR_CLOSED(curr_state, prev_state))
		{
			syslog(LOG_INFO, "%ld: Door closed\n", timestamp);

			/* Store the most recent time the door closed */
			pthread_mutex_lock(&closed_door_mutex);
			latest_closed_door_tv = timeval;
			pthread_mutex_unlock(&closed_door_mutex);

			if (SWITCH_ISON(MASTER_SWITCH_PIN) ||
					SWITCH_ISON(ALERT_SWITCH_PIN))
			{
				/* Notify by email */
				arg = alloc_arg(timeval, "closed");
				pthread_create(&tid, &attr, &notify_by_mail,
						(void *)arg);
			}
		}
		else if (DOOR_OPENED(curr_state, prev_state))
		{
			syslog(LOG_INFO, "%ld: Door opened\n", timestamp);
			if (SWITCH_ISON(MASTER_SWITCH_PIN) ||
					SWITCH_ISON(ALERT_SWITCH_PIN))
			{
				/* Sound buzzer */
				buzzer_arg = alloc_arg(timeval, "opened");
				pthread_create(&tid, &attr, &hit_buzzer,
						(void *)buzzer_arg);
				/* Notify by email */
				arg = alloc_arg(timeval, "opened");
				pthread_create(&tid, &attr, &notify_by_mail,
						(void *)arg);
			}
		}
		prev_state = curr_state;
		delay((unsigned) 100);
	}

	syslog(LOG_INFO, "Termination completed\n");

	pthread_attr_destroy(&attr);
	closelog();

	return (EXIT_SUCCESS);
}


/* Get current time, store the corresponding timeval_t where tv_ptr
 * points to and return timestamp in seconds.
 */
long get_curr_time(timeval_t *tv_ptr)
{
	if (!tv_ptr)
	{
		syslog(LOG_INFO, "get_curr_time: NULL parameter provided\n");
		pthread_kill(tids[MAIN_THREAD], SIGUSR1);
	}

	gettimeofday(tv_ptr, NULL);
	return get_timestamp(*tv_ptr);
}


/* Convert timeval_t to timestamp in seconds */
long get_timestamp(timeval_t tv)
{
	return tv.tv_sec + (tv.tv_usec * 1.0E-06);
}


/* Allocate thread argument */
thrarg_t *alloc_arg(timeval_t tv, char *state)
{
	thrarg_t *arg;

	arg = (thrarg_t *) malloc(sizeof(thrarg_t));
	if (!arg)
	{
		syslog(LOG_INFO, "malloc: %s", strerror(errno));
		return NULL;
	}
	arg->timeval = tv;
	arg->state = state;
	return arg;
}


/* Executed by detached thread */
void *notify_by_mail(void *arg)
{
	char cmd[256], *state;
	int  ret, attempts;
	long timestamp;

	if (!arg)
		return ((void *) EXIT_FAILURE);

	timestamp = get_timestamp(((thrarg_t *) arg)->timeval);
	state = ((thrarg_t *) arg)->state;

	snprintf(cmd, 256, "%s/door-sendmail.sh %s %ld", STR(TARGET_DIR),
		state, timestamp);
	for (attempts = 10; attempts > 0; attempts--)
	{
		ret = system(cmd);
		if (ret != -1 && WIFEXITED(ret))
		{
			ret = WEXITSTATUS(ret);
			if (ret == 0)
			{
				syslog(LOG_INFO, "%ld: Mail sent (%s)\n",
					timestamp, state);
				free(arg);
				return ((void *) EXIT_SUCCESS);
			}
		}
		syslog(LOG_INFO, "%ld: Attempt to send mail #%d/9\n",
			timestamp, 10-attempts);
		pthread_yield();
		delay((unsigned) 500);
	}
	free(arg);

	return ((void *) EXIT_FAILURE);
}


/* Signal handler for SIGINT, SIGTERM and SIGUSR1 */
void request_termination(int signo)
{
	/* Make sure only the main thread accesses exit_signal
	 * and termination_requested variables, as no synchronization
	 * can take place inside the signal handler to avoid race conditions.
	 */
	if (pthread_self() == tids[MAIN_THREAD])
	{
		exit_signal = signo;
		termination_requested = 1;
	}
	else
		pthread_kill(tids[MAIN_THREAD], SIGUSR1);
}


/* Executed by detached thread */
void *hit_buzzer(void *arg)
{
	int       i;
	timeval_t opendoor_tv;
	long      opendoor_time;

	if (!arg)
		return ((void *) EXIT_FAILURE);

	opendoor_tv = ((thrarg_t *) arg)->timeval;
	opendoor_time = get_timestamp(opendoor_tv);

	/* Sound the buzzer three (3) times for one (1) second */
	for (i = 0; i < 3; i++)
	{
		digitalWrite(BUZZER_PIN, HIGH);
		delay((unsigned) 1000);
		digitalWrite(BUZZER_PIN, LOW);
		delay((unsigned) 500);
	}


	for (i = 0; ; i++)
	{
		/* Check if the door has closed since this hit_buzzer instance
		 * was called. Variable latest_closed_door_tv is shared between
		 * all threads and synchronization is required to access it.
		 */
		pthread_mutex_lock(&closed_door_mutex);
		if (timevalcmp(opendoor_tv, latest_closed_door_tv) < 0)
		{
			pthread_mutex_unlock(&closed_door_mutex);
			break;
		}
		pthread_mutex_unlock(&closed_door_mutex);

		/* Sound the buzzer one (1) time for five (5) seconds every
		 * approximately five (5) minutes as a reminder that the door
		 * is still open.
		 */
		if (i == 60*5)
		{
			syslog(LOG_INFO, "%ld: Door is still open\n", opendoor_time);
			digitalWrite(BUZZER_PIN, HIGH);
			delay((unsigned) 1000*5);
			digitalWrite(BUZZER_PIN, LOW);
			i = 0;
		}
		delay((unsigned) 1000);
	}
	free(arg);

	return ((void *) EXIT_SUCCESS);
}


/* Precise timeval_t comparator */
int timevalcmp(timeval_t tv1, timeval_t tv2)
{
	if (tv1.tv_sec < tv2.tv_sec)
		return (-1);
	else if (tv1.tv_sec > tv2.tv_sec)
		return (1);
	else if (tv1.tv_usec < tv2.tv_usec)
		return (-1);
	else if (tv1.tv_usec > tv2.tv_usec)
		return (1);
	return (0);
}

