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
	long req_time;
	char *state;
} thrarg_t;

/* Function prototypes */
long      get_timestamp(void);
thrarg_t *alloc_arg(long req_time, char *state);
void     *notify_by_mail(void *arg);
void      terminate(int signo);
void     *hit_buzzer(void *arg);

/* Global declarations */
volatile sig_atomic_t exitflag = 0;


int main(void)
{
	volatile int    curr_state,
	                prev_state = -1;
	pthread_attr_t  attr;
	pthread_t       tid;
	thrarg_t       *arg;
	long            req_time;

	openlog("door-monitor", LOG_PID, LOG_USER);
	signal(SIGTERM, &terminate);
	signal(SIGINT,  &terminate);

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

	wiringPiSetup();
	if (piHiPri(99))
		syslog(LOG_INFO, "piHiPri: %s", strerror(errno));

	pinMode(REED_SENSOR_PIN, INPUT);
	pinMode(BUZZER_PIN, OUTPUT);

	for (; exitflag == 0;)
	{
		curr_state = digitalRead(REED_SENSOR_PIN);
		if (DOOR_CLOSED(curr_state, prev_state))
		{
			req_time = get_timestamp();
			syslog(LOG_INFO, "%ld: Door closed\n", req_time);
			arg = alloc_arg(req_time, "closed");
			pthread_create(&tid, &attr, &notify_by_mail, (void *)arg);
		}
		if (DOOR_OPENED(curr_state, prev_state))
		{
			req_time = get_timestamp();
			syslog(LOG_INFO, "%ld: Door opened\n", req_time);
			pthread_create(&tid, &attr, &hit_buzzer, NULL);
			arg = alloc_arg(req_time, "opened");
			pthread_create(&tid, &attr, &notify_by_mail, (void *)arg);
		}
		prev_state = curr_state;
		delay((unsigned) 100);
	}

	syslog(LOG_INFO, "Termination requested (%s)", strsignal(exitflag));
	pthread_attr_destroy(&attr);
	closelog();

	return (EXIT_SUCCESS);
}


long get_timestamp()
{
	timeval_t tv;
	long req_time;

	gettimeofday(&tv, NULL);
	req_time = (long) tv.tv_sec + (tv.tv_usec)*1.0E-06;
	return req_time;
}


thrarg_t *alloc_arg(long req_time, char *state)
{
	thrarg_t *arg;

	arg = (thrarg_t *) malloc(sizeof(thrarg_t));
	if (!arg)
	{
		syslog(LOG_INFO, "malloc: %s", strerror(errno));
		return NULL;
	}
	arg->req_time = req_time;
	arg->state = state;
	return arg;
}


void *notify_by_mail(void *arg)
{
	char      cmd[256], *state;
	int       ret, attempts;
	long      req_time;

	if (!arg)
		return ((void *) EXIT_FAILURE);

	req_time = ((thrarg_t *) arg)->req_time;
	state    = ((thrarg_t *) arg)->state;

	snprintf(cmd, 256, "%s/door-sendmail.sh %s %ld", STR(TARGET_DIR),
		state, req_time);
	for (attempts = 10; attempts > 0; attempts--)
	{
		ret = system(cmd);
		if (ret != -1 && WIFEXITED(ret))
		{
			ret = WEXITSTATUS(ret);
			if (ret == 0)
			{
				syslog(LOG_INFO, "%ld: Mail sent (%s)\n",
					req_time, state);
				free(arg);
				return ((void *) EXIT_SUCCESS);
			}
		}
		syslog(LOG_INFO, "%ld: Attempt to send mail #%d/9\n",
			req_time, 10-attempts);
		pthread_yield();
		delay((unsigned) 500);
	}
	free(arg);

	return ((void *) EXIT_FAILURE);
}


void terminate(int signo)
{
	exitflag = signo;
}


void *hit_buzzer(void *arg)
{
	int i;
	for (i = 0; i < 3; i++)
	{
		digitalWrite(BUZZER_PIN, HIGH);
		delay((unsigned) 1000);
		digitalWrite(BUZZER_PIN, LOW);
		delay((unsigned) 500);
	}

	return ((void *) EXIT_SUCCESS);
}

