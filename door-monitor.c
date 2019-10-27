/*
 * +-----------------------------------------------------------------------+
 * |             Copyright (C) 2017-2018 George Z. Zachos                  |
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
#define MAIN_THREAD                      0
#define REED_SENSOR_PIN                  0
#define BUZZER_PIN                       13
#define DOOR_IS_CLOSED(state)            (state == LOW)
#define DOOR_WAS_OPEN(state)             (state == HIGH)
#define DOOR_WAS_CLOSED(state)           (state == LOW)
#define DOOR_CLOSED(state,prev_state) \
                (DOOR_WAS_OPEN(prev_state) && DOOR_IS_CLOSED(state))
#define DOOR_OPENED(state,prev_state) \
                (DOOR_WAS_CLOSED(prev_state) && !DOOR_IS_CLOSED(state))

typedef struct timeval timeval_t;
typedef struct thrarg_s {
	timeval_t  req_timeval;
	char      *state;
} thrarg_t;

/* Function prototypes */
long      get_timestamp(timeval_t tv);
thrarg_t *alloc_arg(timeval_t req_timeval, char *state);
void     *notify_by_mail(void *arg);
void      request_termination(int signo);
void      terminate(int signo);
void     *hit_buzzer(void *arg);
int       timevalcmp(timeval_t tv1, timeval_t tv2);

/* Global declarations */
volatile sig_atomic_t termination_requested = 0, exit_signal = 0;
volatile int exitflag = 0;
volatile timeval_t latest_closed_door;
pthread_mutex_t exitflag_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_t tids[1];

int main(void)
{
	volatile int    curr_state,
	                prev_state = -1;
	pthread_attr_t  attr;
	pthread_t       tid;
	thrarg_t       *arg, *buzzer_arg;
	timeval_t       req_timeval;
	long            req_timestamp;

	openlog("door-monitor", LOG_PID, LOG_USER);
	signal(SIGTERM, &request_termination);
	signal(SIGINT,  &request_termination);
	signal(SIGUSR1, &terminate);

	tids[MAIN_THREAD] = pthread_self();
	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	wiringPiSetup();
	if (piHiPri(99))
		syslog(LOG_INFO, "piHiPri: %s", strerror(errno));

	pinMode(REED_SENSOR_PIN, INPUT);
	pinMode(BUZZER_PIN, OUTPUT);

	for (; ;)
	{
		if (termination_requested)
		{
			if (pthread_mutex_trylock(&exitflag_mutex) == 0)
			{
				exitflag = termination_requested;
				pthread_mutex_unlock(&exitflag_mutex);
				break;
			}
		}
		curr_state = digitalRead(REED_SENSOR_PIN);
		gettimeofday(&req_timeval, NULL);
		req_timestamp = get_timestamp(req_timeval);
		if (DOOR_CLOSED(curr_state, prev_state))
		{
			latest_closed_door = req_timeval;
			syslog(LOG_INFO, "%ld: Door closed\n", req_timestamp);
			arg = alloc_arg(req_timeval, "closed");
			pthread_create(&tid, &attr, &notify_by_mail, (void *)arg);
		}
		if (DOOR_OPENED(curr_state, prev_state))
		{
			syslog(LOG_INFO, "%ld: Door opened\n", req_timestamp);
			buzzer_arg = alloc_arg(req_timeval, "opened");
			pthread_create(&tid, &attr, &hit_buzzer, (void *)buzzer_arg);
			arg = alloc_arg(req_timeval, "opened");
			pthread_create(&tid, &attr, &notify_by_mail, (void *)arg);
		}
		prev_state = curr_state;
		delay((unsigned) 100);
	}

	pthread_mutex_lock(&exitflag_mutex);
	syslog(LOG_INFO, "Termination requested (%s)", strsignal(exit_signal));
	pthread_mutex_unlock(&exitflag_mutex);

	pthread_attr_destroy(&attr);
	closelog();

	return (EXIT_SUCCESS);
}


long get_timestamp(timeval_t tv)
{
	return ((long) tv.tv_sec + (tv.tv_usec)*1.0E-06);
}


thrarg_t *alloc_arg(timeval_t req_timeval, char *state)
{
	thrarg_t *arg;

	arg = (thrarg_t *) malloc(sizeof(thrarg_t));
	if (!arg)
	{
		syslog(LOG_INFO, "malloc: %s", strerror(errno));
		return NULL;
	}
	arg->req_timeval = req_timeval;
	arg->state = state;
	return arg;
}


void *notify_by_mail(void *arg)
{
	char      cmd[256], *state;
	int       ret, attempts;
	long      req_timestamp;

	if (!arg)
		return ((void *) EXIT_FAILURE);

	req_timestamp = get_timestamp(((thrarg_t *) arg)->req_timeval);
	state = ((thrarg_t *) arg)->state;

	snprintf(cmd, 256, "%s/door-sendmail.sh %s %ld", STR(TARGET_DIR),
		state, req_timestamp);
	for (attempts = 10; attempts > 0; attempts--)
	{
		ret = system(cmd);
		if (ret != -1 && WIFEXITED(ret))
		{
			ret = WEXITSTATUS(ret);
			if (ret == 0)
			{
				syslog(LOG_INFO, "%ld: Mail sent (%s)\n",
					req_timestamp, state);
				free(arg);
				return ((void *) EXIT_SUCCESS);
			}
		}
		syslog(LOG_INFO, "%ld: Attempt to send mail #%d/9\n",
			req_timestamp, 10-attempts);
		pthread_yield();
		delay((unsigned) 500);
	}
	free(arg);

	return ((void *) EXIT_FAILURE);
}


void request_termination(int signo)
{
	exit_signal = signo;
	if (pthread_self() == tids[MAIN_THREAD])
		terminate(SIGUSR1);
	else
		pthread_kill(tids[MAIN_THREAD], SIGUSR1);
}


void terminate(int signo)
{
	termination_requested = 1;
}


void *hit_buzzer(void *arg)
{
	int i;
	long      req_timestamp;
	timeval_t opendoor_time;

	if (!arg)
		return ((void *) EXIT_FAILURE);

	opendoor_time = ((thrarg_t *) arg)->req_timeval;
	req_timestamp = get_timestamp(opendoor_time);

	for (i = 0; i < 3; i++)
	{
		digitalWrite(BUZZER_PIN, HIGH);
		delay((unsigned) 1000);
		digitalWrite(BUZZER_PIN, LOW);
		delay((unsigned) 500);
	}

	for (i = 0; timevalcmp(latest_closed_door, opendoor_time) == -1; i++)
	{
		if (i == 60*5)
		{
			syslog(LOG_INFO, "%ld: Door is still open\n", req_timestamp);
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


