/**
 * @file phc2sys.c
 * @brief Utility program to synchronize two clocks via a PPS.
 * @note Copyright (C) 2012 Richard Cochran <richardcochran@gmail.com>
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
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <linux/pps.h>
#include <linux/ptp_clock.h>

#include "missing.h"

#define KP 0.7
#define KI 0.3
#define NS_PER_SEC 1000000000LL

#define max_ppb  512000
#define min_ppb -512000

static clockid_t clock_open(char *device)
{
	int fd = open(device, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "cannot open %s: %m\n", device);
		return CLOCK_INVALID;
	}
	return FD_TO_CLOCKID(fd);
}

static void clock_ppb(clockid_t clkid, double ppb)
{
	struct timex tx;
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_FREQUENCY;
	tx.freq = (long) (ppb * 65.536);
	if (clock_adjtime(clkid, &tx) < 0)
		fprintf(stderr, "failed to adjust the clock: %m\n");
}

static void clock_step(clockid_t clkid, int64_t ns)
{
	struct timex tx;
	int sign = 1;
	if (ns < 0) {
		sign = -1;
		ns *= -1;
	}
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_SETOFFSET | ADJ_NANO;
	tx.time.tv_sec  = sign * (ns / NS_PER_SEC);
	tx.time.tv_usec = sign * (ns % NS_PER_SEC);
	/*
	 * The value of a timeval is the sum of its fields, but the
	 * field tv_usec must always be non-negative.
	 */
	if (tx.time.tv_usec < 0) {
		tx.time.tv_sec  -= 1;
		tx.time.tv_usec += 1000000000;
	}
	if (clock_adjtime(clkid, &tx) < 0)
		fprintf(stderr, "failed to step clock: %m\n");
}

static int read_phc(clockid_t clkid, clockid_t sysclk, int rdelay, int64_t *offset, uint64_t *ts)
{
	struct timespec tsrc, tdst;

	if (clkid == CLOCK_INVALID) {
		return 0;
	}

	if (clock_gettime(clkid, &tsrc)) {
		perror("clock_gettime");
		return 0;
	}

	if (clock_gettime(sysclk, &tdst)) {
		perror("clock_gettime");
		return 0;
	}

	*offset = tdst.tv_sec * NS_PER_SEC - tsrc.tv_sec * NS_PER_SEC +
		tdst.tv_nsec - tsrc.tv_nsec - rdelay;
	*ts = tdst.tv_sec * NS_PER_SEC + tdst.tv_nsec;

	return 1;
}

struct servo {
	uint64_t last_ts;
	double drift;
	enum {
		SAMPLE_0, SAMPLE_1, SAMPLE_2, SAMPLE_3, SAMPLE_N
	} state;
};

static struct servo servo;

static void do_servo(struct servo *srv,
		     clockid_t src, clockid_t dst,
		     int64_t offset, uint64_t ts, double kp, double ki)
{
	double ki_term, ppb;
	int64_t delta;

	printf("s%d %lld.%09llu drift %.2f\n",
		srv->state, ts / NS_PER_SEC, ts % NS_PER_SEC, srv->drift);

	switch (srv->state) {
	case SAMPLE_0:
		clock_ppb(dst, 0.0);
		srv->state = SAMPLE_1;
		break;
	case SAMPLE_1:
		srv->state = SAMPLE_2;
		break;
	case SAMPLE_2:
		delta = ts - srv->last_ts;
		offset = delta - NS_PER_SEC;
		srv->drift = offset;
		clock_ppb(dst, -offset);
		srv->state = SAMPLE_3;
		break;
	case SAMPLE_3:
		clock_step(dst, -offset);
		srv->state = SAMPLE_N;
		break;
	case SAMPLE_N:
		ki_term = ki * offset;
		ppb = kp * offset + srv->drift + ki_term;
		if (ppb < min_ppb) {
			ppb = min_ppb;
		} else if (ppb > max_ppb) {
			ppb = max_ppb;
		} else {
			srv->drift += ki_term;
		}
		clock_ppb(dst, -ppb);
		break;
	}

	srv->last_ts = ts;
}

static int read_pps(int fd, int64_t *offset, uint64_t *ts)
{
	struct pps_fdata pfd;

	pfd.timeout.sec = 10;
	pfd.timeout.nsec = 0;
	pfd.timeout.flags = ~PPS_TIME_INVALID;
	if (ioctl(fd, PPS_FETCH, &pfd)) {
		perror("ioctl PPS_FETCH");
		return 0;
	}

	*ts = pfd.info.assert_tu.sec * NS_PER_SEC;
	*ts += pfd.info.assert_tu.nsec;

	*offset = *ts % NS_PER_SEC;
	if (*offset > NS_PER_SEC / 2)
		*offset -= NS_PER_SEC;

	return 1;
}

static void usage(char *progname)
{
	fprintf(stderr,
		"\n"
		"usage: %s [options]\n\n"
		" -c [device]  slave clock device, default CLOCK_REALTIME\n"
		" -d [device]  master device, source of PPS events\n"
		" -h           prints this message and exits\n"
		" -r [val]     reading the PHC device takes 'val' nanoseconds\n"
		" -s [device]  set the time from this PHC device\n"
		" -P [val]     set proportional constant to 'val'\n"
		" -I [val]     set integration constant to 'val'\n"
		"\n",
		progname);
}

int main(int argc, char *argv[])
{
	double kp = KP, ki = KI;
	char *device = NULL, *progname;
	clockid_t src = CLOCK_INVALID, dst = CLOCK_REALTIME;
	uint64_t pps_ts, phc_ts;
	int64_t pps_offset, phc_offset;
	int c, fd = 0, rdelay = 0;

	/* Process the command line arguments. */
	progname = strrchr(argv[0], '/');
	progname = progname ? 1+progname : argv[0];
	while (EOF != (c = getopt(argc, argv, "c:d:hr:s:P:I:"))) {
		switch (c) {
		case 'c':
			dst = clock_open(optarg);
			break;
		case 'd':
			device = optarg;
			break;
		case 'r':
			rdelay = atoi(optarg);
			break;
		case 's':
			src = clock_open(optarg);
			break;
		case 'P':
			kp = atof(optarg);
			break;
		case 'I':
			ki = atof(optarg);
			break;
		case 'h':
			usage(progname);
			return 0;
		default:
			usage(progname);
			return -1;
		}
	}

	if (!(device || src != CLOCK_INVALID) || dst == CLOCK_INVALID) {
		usage(progname);
		return -1;
	}
	if (device) {
		fd = open(device, O_RDONLY);
		if (fd < 0) {
			fprintf(stderr, "cannot open %s: %m\n", device);
			return -1;
		}
	}
	if (src != CLOCK_INVALID) {
		struct timespec now;
		if (clock_gettime(src, &now))
			perror("clock_gettime");
		if (clock_settime(dst, &now))
			perror("clock_settime");
	}
	while (1) {
		if (fd > 0) {
			if (!read_pps(fd, &pps_offset, &pps_ts))
				continue;
			printf("pps %9lld ", pps_offset);
		} else
			usleep(1000000);

		if (!read_phc(src, dst, rdelay, &phc_offset, &phc_ts))
			continue;
		printf("phc %9lld ", phc_offset);

		if (fd > 0)
			do_servo(&servo, src, dst, pps_offset, pps_ts, kp, ki);
		else
			do_servo(&servo, src, dst, phc_offset, phc_ts, kp, ki);
	}
	return 0;
}
