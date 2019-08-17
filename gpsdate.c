/* gpsdate - small utility to set system RTC based on gpsd time 
 * (C) 2013-2019 by sysmocom - s.f.m.c. GmbH, Author: Harald Welte
 * All Rights Reserved
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/* The idea of this program is that you run it once at system boot time,
 * to set the local RTC to the time received by GPS.  Further synchronization
 * during system runtime is then handled by ntpd, interfacing with gpsd using
 * the ntp shared memory protocol.
 *
 * However, ntpd is unable to accept a GPS time that's off by more than four
 * hours from the system RTC, so initial synchronization has to be done
 * externally.  'ntpdate' is the usual option, but doesn't work if you're
 * offline.  Thus, this gpsdate utilith was created to fill the gap.
 */

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <syslog.h>
#include <getopt.h>
#include <inttypes.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <gps.h>

#define NUM_RETRIES	60	/* Number of gpsd re-connects */
#define RETRY_SLEEP	1	/* Seconds to sleep between re-connects */

static int no_detach = 0;
static struct gps_data_t gpsdata;

static void callback(struct gps_data_t *gpsdata)
{
	struct timeval tv;
	time_t time;
	char *timestr, *lf;
	int rc;

	if (!(gpsdata->set & TIME_SET))
		return;

	tv.tv_sec = gpsdata->fix.time;
	/* FIXME: use the fractional part for microseconds */
	tv.tv_usec = 0;

	time = tv.tv_sec;
	timestr = ctime(&time);
	if (!timestr) {
		syslog(LOG_ERR, "ctime failed");
		timestr = "<unknown>";
	}
	/* god knows why ctime insists on including a LF at the end */
	lf = strchr(timestr, '\n');
	if (lf)
		*lf = '\0';

	syslog(LOG_DEBUG, "%s: gpsdate->set=0x%08"PRIu64"x status=%u sats_used=%u\n",
		timestr, gpsdata->set, gpsdata->status, gpsdata->satellites_used);

	if (gpsdata->status == 0) {
		syslog(LOG_INFO, "%s: discarding; no fix yet\n", timestr);
		return;
	}

	if (gpsdata->satellites_used == 0) {
		syslog(LOG_INFO, "%s: discarding; 0 satellites used\n", timestr);
		return;
	}

	rc = settimeofday(&tv, NULL);
	gps_close(gpsdata);
	if (rc == 0) {
		syslog(LOG_NOTICE, "Successfully set RTC time to GPSD time:"
			" %s\n", timestr);
		closelog();
		exit(EXIT_SUCCESS);
	} else {
		syslog(LOG_ERR, "Error setting RTC: %d (%s)\n",
			errno, strerror(errno));
		closelog();
		exit(EXIT_FAILURE);
	}
}

static int osmo_daemonize(void)
{
	int rc;
	pid_t pid, sid;

	/* Check if parent PID == init, in which case we are already a daemon */
	if (getppid() == 1)
		return -EEXIST;

	/* Fork from the parent process */
	pid = fork();
	if (pid < 0) {
		/* some error happened */
		return pid;
	}

	if (pid > 0) {
		/* if we have received a positive PID, then we are the parent
		 * and can exit */
		exit(0);
	}

	/* FIXME: do we really want this? */
	umask(0);

	/* Create a new session and set process group ID */
	sid = setsid();
	if (sid < 0)
		return sid;

	/* Change to the /tmp directory, which prevents the CWD from being locked
	 * and unable to remove it */
	rc = chdir("/tmp");
	if (rc < 0)
		return rc;

	/* Redirect stdio to /dev/null */
/* since C89/C99 says stderr is a macro, we can safely do this! */
#ifdef stderr
	freopen("/dev/null", "r", stdin);
	freopen("/dev/null", "w", stdout);
	freopen("/dev/null", "w", stderr);
#endif

	return 0;
}

/* local copy, as the libgps official version ignores gps_read() result */
static int my_gps_mainloop(struct gps_data_t *gdata,
			   int timeout,
			   void (*hook)(struct gps_data_t *gdata))
{
	int rc;

	for (;;) {
		if (!gps_waiting(gdata, timeout)) {
			return -1;
		} else {
			rc = gps_read(gdata);
			if (rc < 0)
				return rc;
			(*hook)(gdata);
		}
	}
	return 0;
}

static int attempt_reconnect(const char *host, const char *port,
			     struct gps_data_t *gpsdata)
{
	int rc;

	rc = gps_open(host, port, gpsdata);
	if (rc)
		return -1;

	syslog(LOG_INFO, "(re)connected to gpsd\n");

	gps_stream(gpsdata, WATCH_ENABLE|WATCH_JSON, NULL);

	return 0;
}

enum state {
	S_CONNECTED,
	S_RECONNECT,
};

int main(int argc, char **argv)
{
	char *host = "localhost";
	char *port = DEFAULT_GPSD_PORT;
	int num_retries = NUM_RETRIES;
	int retry_sleep = RETRY_SLEEP;
	int i, rc;
	enum state state;

	openlog("gpsdate", LOG_PERROR, LOG_CRON);

	while (1) {
		int option_index = 0, c;
		static struct option long_options[] = {
			{"num-retries", 1, 0, 'n'},
			{"retry-sleep", 1, 0, 's'},
			{"no-detach", 0, 0, 'd'},
			{0,0,0,0}
		};

		c = getopt_long(argc, argv, "n:s:d",
				long_options, &option_index);
		if (c == -1)
			break;

		switch (c) {
		case 'n':
			num_retries = atoi(optarg);
			break;
		case 's':
			retry_sleep = atoi(optarg);
			break;
		case 'd':
			no_detach = 1;
			break;
		}
	}

	if (optind < argc)
		host = argv[optind++];
	if (optind < argc)
		port = argv[optind++];

	/* attempt up to NUM_RETRIES times to connect to gpsd while we are
	 * still running in foreground.  The idea is that we will block the
	 * boot process (init scripts) until we have a connection */
	for (i = 1; i <= num_retries; i++) {
		printf("Attempt #%d to connect to gpsd at %s...\n", i, host);
		rc = attempt_reconnect(host, port, &gpsdata);
		if (rc >= 0)
			break;
		sleep(retry_sleep);
	}

	if (rc < 0) {
		syslog(LOG_ERR, "no gpsd running or network error: %d, %s\n",
			errno, gps_errstr(errno));
		closelog();
		exit(EXIT_FAILURE);
	}
	state = S_CONNECTED;

	if (!no_detach)
		osmo_daemonize();

	/* We run in an endless loop.  The only reasonable way to exit is after
	 * a correct GPS timestamp has been received in callback() */
	while (1) {
		switch (state) {
		case S_CONNECTED:
			rc = my_gps_mainloop(&gpsdata, INT_MAX, callback);
			if (rc < 1) {
				syslog(LOG_ERR, "connection to gpsd was "
					"closed: %d, reconnecting\n", rc);
				gps_close(&gpsdata);
				state = S_RECONNECT;
			}
			break;
		case S_RECONNECT:
			rc = attempt_reconnect(host, port, &gpsdata);
			if (rc < 0)
				sleep(RETRY_SLEEP);
			else
				state = S_CONNECTED;
			break;
		}
	}

	gps_close(&gpsdata);

	closelog();
	exit(EXIT_SUCCESS);
}
