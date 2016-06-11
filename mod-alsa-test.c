/* MOD - ALSA test utility
 *
 * Copyright (C) 2016 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#ifndef VERSION
#define VERSION "v0.1"
#endif

#define _GNU_SOURCE

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include <signal.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <alsa/asoundlib.h>

typedef struct  {
	/* settings */
	unsigned int       samplerate;
	snd_pcm_uframes_t  samples_per_period;
	unsigned int       play_periods_per_cycle;
	unsigned int       capt_periods_per_cycle;
	unsigned int       play_nchan;
	unsigned int       capt_nchan;

	float              run_for;
	bool               debug;

	float**            testbuffers;

	/* state */
	snd_pcm_t* play_handle;
	snd_pcm_t* capt_handle;
	bool       synced;

	char*             play_ptr [64];
	const char*       capt_ptr [64];
	snd_pcm_uframes_t capt_offset;
	snd_pcm_uframes_t play_offset;
	size_t            play_bytes_per_sample;
	size_t            capt_bytes_per_sample;

	int play_step;
	int capt_step;

	int play_npfd;
	int capt_npfd;
} AlsaIO;

static volatile bool signalled = false;

void handle_sig (int sig) {
	fprintf (stdout,"caught signal - shutting down.\n");
	signalled = true;
	signal (sig, SIG_DFL);
}


static int realtime_pthread_create (
		const int policy,
		int priority,
		const size_t stacksize,
		pthread_t *thread,
		void *(*start_routine) (void *),
		void *arg)
{
	int rv;

	pthread_attr_t attr;
	struct sched_param parm;

	const int p_min = sched_get_priority_min (policy);
	const int p_max = sched_get_priority_max (policy);
	priority += p_max;
	if (priority > p_max) {
		priority = p_max;
	}
	if (priority < p_min) {
		priority = p_min;
	}
	parm.sched_priority = priority;

	pthread_attr_init (&attr);
	pthread_attr_setschedpolicy (&attr, policy);
	pthread_attr_setschedparam (&attr, &parm);
	pthread_attr_setscope (&attr, PTHREAD_SCOPE_SYSTEM);
	pthread_attr_setinheritsched (&attr, PTHREAD_EXPLICIT_SCHED);
	pthread_attr_setstacksize (&attr, stacksize);
	rv = pthread_create (thread, &attr, start_routine, arg);
	pthread_attr_destroy (&attr);
	return rv;
}


static int set_hwpar (AlsaIO* io, snd_pcm_hw_params_t *hwpar, bool play)
{
	bool err;
	snd_pcm_t* handle;
	const char* errname;
	unsigned int ppc;
	unsigned int max_chan = 0;
	unsigned int *nchan = 0;

	if (play) {
		handle  = io->play_handle;
		errname = "playback";
		ppc     = io->play_periods_per_cycle;
		nchan   = &io->play_nchan;
	} else {
		handle  = io->capt_handle;
		errname = "capture";
		ppc     = io->capt_periods_per_cycle;
		nchan   = &io->capt_nchan;
	}

	if (snd_pcm_hw_params_any (handle, hwpar) < 0) {
		fprintf (stderr, "no %s hw configurations available.\n", errname);
		return -1;
	}
	if (snd_pcm_hw_params_set_periods_integer (handle, hwpar) < 0) {
		fprintf (stderr, "cannot set %s period size to integral value.\n", errname);
		return -1;
	}
	if (   (snd_pcm_hw_params_set_access (handle, hwpar, SND_PCM_ACCESS_MMAP_NONINTERLEAVED) < 0)
	    && (snd_pcm_hw_params_set_access (handle, hwpar, SND_PCM_ACCESS_MMAP_INTERLEAVED) < 0)
	    && (snd_pcm_hw_params_set_access (handle, hwpar, SND_PCM_ACCESS_MMAP_COMPLEX) < 0))
	{
		fprintf (stderr, "the %s interface doesn't support mmap-based access.\n", errname);
		return -1;
	}

	err = (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_FLOAT_LE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S32_LE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S32_BE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S24_3LE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S24_3BE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S24_LE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S24_BE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S16_LE) < 0)
		&& (snd_pcm_hw_params_set_format (handle, hwpar, SND_PCM_FORMAT_S16_BE) < 0);

	if (err) {
		fprintf (stderr, "no supported sample format on %s interface.\n.", errname);
		return -1;
	}
	if (snd_pcm_hw_params_set_rate (handle, hwpar, io->samplerate, 0) < 0) {
		fprintf (stderr, "cannot set %s sample rate to %u.\n", errname, io->samplerate);
		return -1;
	}

	snd_pcm_hw_params_get_channels_max (hwpar, &max_chan);
	if (*nchan == 0) {
		*nchan = max_chan;
	}
	if (*nchan > 64) {
		fprintf (stderr, "detected more than 64 %s channnels, reset to 2.\n", errname);
		*nchan = 2;
	}
	if (*nchan < 1) {
		fprintf (stderr, "invalid %s channnel count %d\n", errname, *nchan);
		return 0;
	}

	if (snd_pcm_hw_params_set_channels (handle, hwpar, *nchan) < 0) {
		fprintf (stderr, "cannot set %s channel count to %u.\n", errname, *nchan);
		return -1;
	}
	if (snd_pcm_hw_params_set_period_size (handle, hwpar, io->samples_per_period, 0) < 0) {
		fprintf (stderr, "cannot set %s period size to %lu.\n", errname, io->samples_per_period);
		return -1;
	}
	if (snd_pcm_hw_params_set_periods (handle, hwpar, ppc, 0) < 0) {
		fprintf (stderr, "cannot set %s periods to %u.\n", errname, ppc);
		return -1;
	}
	if (snd_pcm_hw_params_set_buffer_size (handle, hwpar, io->samples_per_period * ppc) < 0) {
		fprintf (stderr, "cannot set %s buffer length to %lu.\n", errname, io->samples_per_period * ppc);
		return -1;
	}
	if (snd_pcm_hw_params (handle, hwpar) < 0) {
		fprintf (stderr, "cannot set %s hardware parameters.\n", errname);
		return -1;
	}

	return 0;
}

static int set_swpar (AlsaIO* io, snd_pcm_sw_params_t* swpar, bool play)
{
	int err;

	snd_pcm_t* handle;
	const char* errname;

	if (play) {
		handle  = io->play_handle;
		errname = "playback";
	} else {
		handle  = io->capt_handle;
		errname = "capture";
	}

	snd_pcm_sw_params_current (handle, swpar);

	if ((err = snd_pcm_sw_params_set_tstamp_mode (handle, swpar, SND_PCM_TSTAMP_MMAP)) < 0) {
		fprintf (stderr, "cannot set %s timestamp mode to %u.\n", errname, SND_PCM_TSTAMP_MMAP);
		return -1;
	}
	if ((err = snd_pcm_sw_params_set_avail_min (handle, swpar, io->samples_per_period)) < 0) {
		fprintf (stderr, "cannot set %s avail_min to %lu.\n", errname, io->samples_per_period);
		return -1;
	}
	if ((err = snd_pcm_sw_params (handle, swpar)) < 0) {
		fprintf (stderr, "cannot set %s software parameters.\n", errname);
		return -1;
	}

	return 0;
}

static float xrun_time (snd_pcm_status_t* stat)
{
	struct timeval tupd, trig;
	int            ds, du;
	if (snd_pcm_status_get_state (stat) == SND_PCM_STATE_XRUN) {
		snd_pcm_status_get_tstamp (stat, &tupd);
		snd_pcm_status_get_trigger_tstamp (stat, &trig);
		ds = tupd.tv_sec - trig.tv_sec;
		du = tupd.tv_usec - trig.tv_usec;
		if (du < 0) {
			du += 1000000;
			ds -= 1;
		}
		return ds + 1e-6f * du;
	}
	return 0.0f;
}

static int play_done (AlsaIO* io, int len)
{
	if (!io->play_handle) return 0;
	return snd_pcm_mmap_commit (io->play_handle, io->play_offset, len);
}

static int capt_done (AlsaIO* io, int len)
{
	if (!io->capt_handle) return 0;
	return snd_pcm_mmap_commit (io->capt_handle, io->capt_offset, len);
}


static void clear_chan (AlsaIO* io, char *dst, snd_pcm_uframes_t len)
{
	while (len--) {
		*((int *) dst) = 0;
		dst += io->play_step;
	}
}

static int play_init (AlsaIO* io, snd_pcm_uframes_t len)
{
	int err;
	unsigned int i;
	const snd_pcm_channel_area_t* a;

	if (!io->play_handle) {
		return 0;
	}
	if ((err = snd_pcm_mmap_begin (io->play_handle, &a, &io->play_offset, &len)) < 0) {
		fprintf (stderr, "snd_pcm_mmap_begin (play): %s.\n", snd_strerror (err));
		return -1;
	}
	io->play_step = (a->step) >> 3;
	for (i = 0; i < io->play_nchan; i++, a++) {
		io->play_ptr [i] = (char*) a->addr + ((a->first + a->step * io->play_offset) >> 3);
	}

	return len;
}

static int capt_init (AlsaIO* io, snd_pcm_uframes_t len)
{
	int err;
	unsigned int i;
	const snd_pcm_channel_area_t* a;

	if (!io->capt_handle) {
		return 0;
	}

	if ((err = snd_pcm_mmap_begin (io->capt_handle, &a, &io->capt_offset, &len)) < 0) {
		fprintf (stderr, "snd_pcm_mmap_begin (capt): %s.\n", snd_strerror (err));
		return -1;
	}
	io->capt_step = (a->step) >> 3;
	for (i = 0; i < io->capt_nchan; i++, a++) {
		io->capt_ptr [i] = (char *) a->addr + ((a->first + a->step * io->capt_offset) >> 3);
	}

	return len;
}

static int pcm_start (AlsaIO* io)
{
	int err;
	unsigned int i, j, n;

	if (io->play_handle) {
		n = snd_pcm_avail_update (io->play_handle);
		if (n != io->samples_per_period * io->play_periods_per_cycle) {
			fprintf  (stderr, "full buffer not available at start (%u).\n", n);
			return -1;
		}
		for (i = 0; i < io->play_periods_per_cycle; i++) {
			play_init (io, io->samples_per_period);
			for (j = 0; j < io->play_nchan; j++) {
				clear_chan (io, io->play_ptr [j], io->samples_per_period);
			}
			play_done (io, io->samples_per_period);
		}
		if ((err = snd_pcm_start (io->play_handle)) < 0) {
			fprintf (stderr, "pcm_start (play): %s.\n", snd_strerror (err));
			return -1;
		}
	}
	if (io->capt_handle && !io->synced && ((err = snd_pcm_start (io->capt_handle)) < 0)) {
		fprintf (stderr, "pcm_start (capt): %s.\n", snd_strerror (err));
		return -1;
	}

	return 0;
}

static int pcm_stop (AlsaIO* io)
{
	int err;

	if (io->play_handle && ((err = snd_pcm_drop (io->play_handle)) < 0)) {
		fprintf (stderr, "pcm_drop (play): %s.\n", snd_strerror (err));
		return -1;
	}
	if (io->capt_handle && !io->synced && ((err = snd_pcm_drop (io->capt_handle)) < 0)) {
		fprintf (stderr, "pcm_drop (capt): %s.\n", snd_strerror (err));
		return -1;
	}

	return 0;
}

static int recover (AlsaIO* io)
{
	int err;
	snd_pcm_status_t* stat;
	if (io->debug) {
		printf("recover ()\n");
	}

	snd_pcm_status_alloca (&stat);

	if (io->play_handle) {
		if ((err = snd_pcm_status (io->play_handle, stat)) < 0) {
			fprintf (stderr, "pcm_status (play): %s\n", snd_strerror (err));
		}
		fprintf (stderr, "play x-run %.2f ms\n", 1000.f * xrun_time (stat));
	}

	if (io->capt_handle) {
		if ((err = snd_pcm_status (io->capt_handle, stat)) < 0) {
			fprintf (stderr, "pcm_status (capt): %s\n", snd_strerror (err));
		}
		fprintf (stderr, "capture x-run %.2f ms\n", 1000.f * xrun_time (stat));
	}

	if (pcm_stop (io)) {
		return -1;
	}
	if (io->play_handle && ((err = snd_pcm_prepare (io->play_handle)) < 0)) {
		fprintf (stderr, "pcm_prepare (play): %s\n", snd_strerror (err));
		return -1;
	}
	if (io->capt_handle && !io->synced && ((err = snd_pcm_prepare (io->capt_handle)) < 0)) {
		fprintf (stderr, "pcm_prepare (capt): %s\n", snd_strerror (err));
		return -1;
	}
	if (pcm_start (io)) {
		return -1;
	}

	return 0;
}

static snd_pcm_sframes_t pcm_wait (AlsaIO* io)
{
	bool              need_capt;
	bool              need_play;
	snd_pcm_sframes_t capt_av;
	snd_pcm_sframes_t play_av;
	unsigned short    rev;
	int               i, r, n1, n2;
	struct pollfd     poll_fd [16];

	need_capt = io->capt_handle ? true : false;
	need_play = io->play_handle ? true : false;

	while (need_play || need_capt) {
		n1 = 0;
		if (need_play) {
			snd_pcm_poll_descriptors (io->play_handle, poll_fd, io->play_npfd);
			n1 += io->play_npfd;
		}
		n2 = n1;
		if (need_capt) {
			snd_pcm_poll_descriptors (io->capt_handle, poll_fd + n1, io->capt_npfd);
			n2 += io->capt_npfd;
		}
		for (i = 0; i < n2; i++) {
			poll_fd [i].events |= POLLERR;
		}

		struct timespec timeout;
		timeout.tv_sec = 1;
		timeout.tv_nsec = 0;
		r = ppoll (poll_fd, n2, &timeout, NULL);

		if (r < 0) {
			if (errno == EINTR) return 0;
			fprintf (stderr, "poll (): %s\n.", strerror (errno));
			return 0;
		}
		if (r == 0) {
			fprintf (stderr, "poll timed out.\n");
			return 0;
		}

		if (need_play) {
			snd_pcm_poll_descriptors_revents (io->play_handle, poll_fd, n1, &rev);
			if (rev & POLLERR) {
				fprintf (stderr, "error on playback pollfd.\n");
				recover (io);
				return 0;
			}
			if (rev & POLLOUT) {
				need_play = false;
			}
		}
		if (need_capt) {
			snd_pcm_poll_descriptors_revents (io->capt_handle, poll_fd + n1, n2 - n1, &rev);
			if (rev & POLLERR) {
				fprintf (stderr, "error on capture pollfd.\n");
				recover (io);
				return 0;
			}
			if (rev & POLLIN) {
				need_capt = false;
			}
		}
	}

	play_av = 999999999;
	if (io->play_handle && (play_av = snd_pcm_avail_update (io->play_handle)) < 0) {
		if (io->debug) {
			fprintf (stderr, "play avail %ld\n", play_av);
		}
		recover (io);
		return 0;
	}
	capt_av = 999999999;
	if (io->capt_handle && (capt_av = snd_pcm_avail_update (io->capt_handle)) < 0) {
		if (io->debug) {
			fprintf (stderr, "capt avail %ld\n", capt_av);
		}
		recover (io);
		return 0;
	}

	if (io->debug && io->play_handle && io->capt_handle && capt_av != play_av) {
		fprintf (stderr, "async avail play:%ld capt:%ld\n", play_av, capt_av);
	}

	return (capt_av < play_av) ? capt_av : play_av;
}


void *run_thread (void* arg) {
	AlsaIO * io = arg;

	size_t loop;
	size_t end = io->run_for * io->samplerate / io->samples_per_period;

	for (loop = 0; io->run_for <= 0 || loop < end; ++loop) {
		int c;
		long nr = pcm_wait (io);

		if (io->debug) {
			printf ("proc: %ld\n", nr);
		}
		while (nr >= (long) io->samples_per_period) {
			capt_init (io, io->samples_per_period);
			for (c = 0; c < io->capt_nchan; ++c) {
#if 0
				char const *src = io->capt_ptr [c];
				for (int i = 0; i < io->samples_per_period; ++i) {
					io->testbuffers[c][i] = *((int *) src) / (float)0x7fffff00; // S32LE
					src += io->capt_step;
				}
#endif
			}
			capt_done (io, io->samples_per_period);

			play_init (io, io->samples_per_period);
			for (c = 0; c < io->play_nchan; ++c) {
				char *dst = io->play_ptr [c];
#if 0
				for (int i = 0; i < io->samples_per_period; ++i) {
					*((int *) dst) = io->testbuffers[c][i] * 0x7fffff00; // S32LE
					dst += io->play_step;
				}
#elif 0
				for (int i = 0; i < io->samples_per_period; ++i) {
					*((int *) dst) = sin (M_PI * i / io->samples_per_period) * 0x7fffff00; // S32LE
					dst += io->play_step;
				}
#else
				clear_chan (io, dst, io->samples_per_period);
#endif
			}

			play_done (io, io->samples_per_period);

			nr -= io->samples_per_period;
		}
		if (signalled) {
			break;
		}
	}

	pthread_exit (0);
	return 0;
}

static void usage (int status) {
	printf ("mod-alsa-test - Exercise moddevice.com soundcard\n");
	printf ("Usage: mod-alsa-test [ OPTIONS ]\n");
	// TODO update option...
	printf ("Options:\n\
      -h, --help                 display this help and exit\n\
      -C, --capture <hw:dev>     capture device.\n\
      -d, --device <hw:dev>      set both playback and capture devices.\n\
      -i, --inchannels <num>     number of capture channels.\n\
      -L, --loop <sec>           run for given number of seconds.\n\
      -n, --nperiods <int>,\n\
          --play-periods <int>   playback periods per cycle.\n\
      -N, --capt-nperiods <int>\n\
                                 capture periods per cycle.\n\
      -o, --outchannels <num>    number of playback channels.\n\
      -P, --playback <hw:dev>    playback device.\n\
      -R, --priority <int>       real-time priority (negative) or 0\n\
      -r, --rate <int>           sample rate\n\
      -V, --version              print version information and exit\n\
\n");

	// TODO show defaults, explain loop == 0 etc, give some examples,..
	// and `help2man` format man-page.

	printf ("Report bugs to Robin Gareus <robin@gareus.org>\n");
	exit (status);
}

static const struct option long_options[] = {
	{"capture",      required_argument, 0, 'C'},
	{"device",       required_argument, 0, 'd'},
	{"help",         no_argument,       0, 'h'},
	{"inchannels",   required_argument, 0, 'i'},
	{"loop",         required_argument, 0, 'L'},
	{"nperiods",     required_argument, 0, 'n'},
	{"no-op",        no_argument,       0,  1 },
	{"play-periods", required_argument, 0, 'n'},
	{"capt-periods", required_argument, 0, 'N'},
	{"outchannels",  required_argument, 0, 'o'},
	{"playback",     required_argument, 0, 'P'},
	{"period",       required_argument, 0, 'p'},
	{"priority",     required_argument, 0, 'R'},
	{"rate",         required_argument, 0, 'r'},
	{"version",      no_argument,       0, 'V'}
};

int main (int argc, char** argv)
{
	AlsaIO io;
	unsigned int i;
	unsigned int n_bufs = 0;
	memset (&io, 0, sizeof (io));
	bool sync = true;
	bool noop = false;

	io.samplerate = 48000;
	io.samples_per_period = 128;
	io.play_periods_per_cycle = 2;
	io.capt_periods_per_cycle = 2;
	io.play_nchan = 2;
	io.capt_nchan = 2;
	io.run_for = 10; // seconds
	io.debug = false;

	int rt_priority = -20;

	char *play_device = strdup ("hw:MODDUO");
	char *capt_device = strdup ("hw:MODDUO");

	int c;
	int v;
	while ((c = getopt_long (argc, argv,
			   "C:" /* capture device */
			   "d:" /* devices */
			   "D"  /* */
			   "h"  /* help */
			   "i:" /* input channel count */
			   "L:" /* test-run duration */
			   "n:" /* playback periods */
			   "N:" /* capture periods */
			   "o:" /* output channel count */
			   "P:" /* playback */
			   "p:" /* period/buffer size */
			   "R:" /* realtime priority */
			   "S"  /* */
			   "V", /* version */
			   long_options, (int *) 0)) != EOF) {
		switch (c) {
			case 'h':
				usage (EXIT_SUCCESS);
				break;
			case 'V':
				printf ("mod-alsa-test version %s\n\n", VERSION);
				printf ("Copyright (C) GPL 2016 Robin Gareus <robin@gareus.org>\n");
				exit (0);

			case 'C':
				free (capt_device);
				capt_device = strdup (optarg);
				break;
			case 'd':
				free (capt_device);
				free (play_device);
				capt_device = strdup (optarg);
				play_device = strdup (optarg);
				break;
			case 'D':
				io.debug = true;
				break;
			case 'i':
				v = atoi (optarg);
				if (v < 0) {
					io.capt_nchan = 0; // auto
				} else if (v > 64) {
					io.capt_nchan = 64;
				} else {
					io.capt_nchan = v;
				}
				break;
			case 'L':
				io.run_for = atof (optarg);
				break;
			case 'N':
				v = atoi (optarg);
				if (v < 1) {
					io.capt_periods_per_cycle = 1;
				} else if (v > 32) {
					io.capt_periods_per_cycle = 32;
				} else {
					io.capt_periods_per_cycle = v;
				}
				break;
			case 'n':
				v = atoi (optarg);
				if (v < 1) {
					io.play_periods_per_cycle = 1;
				} else if (v > 32) {
					io.play_periods_per_cycle = 32;
				} else {
					io.play_periods_per_cycle = v;
				}
				break;
			case 'o':
				v = atoi (optarg);
				if (v < 0) {
					io.play_nchan = 0; // auto
				} else if (v > 64) {
					io.play_nchan = 64;
				} else {
					io.play_nchan = v;
				}
				break;
			case 'P':
				free (play_device);
				play_device = strdup (optarg);
				break;
			case 'p':
				v = atoi (optarg);
				if (v < 8) {
					io.samples_per_period = 8;
				} else if (v > 8192) {
					io.samples_per_period = 8192;
				} else {
					io.samples_per_period = v;
				}
				break;
			case 'R':
				rt_priority = atoi (optarg);
				break;
			case 'S':
				sync = false;
				break;
			case 'r':
				v = atoi (optarg);
				if (v < 8000) {
					io.samplerate = 8000;
				} else if (v > 192000) {
					io.samplerate = 192000;
				} else {
					io.samplerate = v;
				}
				break;
			case 1:
				noop = true;
				break;

			default:
			  usage (EXIT_FAILURE);
		}
	}
	/* all systems go */

	int err;
	int rv = -1;
	snd_pcm_hw_params_t* play_hwpar = NULL;
	snd_pcm_sw_params_t* play_swpar = NULL;
	snd_pcm_hw_params_t* capt_hwpar = NULL;
	snd_pcm_sw_params_t* capt_swpar = NULL;

	snd_pcm_format_t play_format;
	snd_pcm_format_t capt_format;
	snd_pcm_access_t play_access;
	snd_pcm_access_t capt_access;

	pthread_t process_thread;

	if (snd_pcm_open (&io.play_handle, play_device, SND_PCM_STREAM_PLAYBACK, 0) < 0) {
		fprintf (stderr, "cannot open playback device '%s'\n", play_device);
	}
	if (snd_pcm_open (&io.capt_handle, capt_device, SND_PCM_STREAM_CAPTURE, 0) < 0) {
		fprintf (stderr, "cannot open capture device '%s'\n", capt_device);
	}
	if (!io.play_handle && !io.capt_handle) {
		fprintf (stderr, "no capture and no playback device.\n");
		goto out;
	}

	if (snd_pcm_hw_params_malloc (&play_hwpar) < 0) {
		fprintf (stderr, "cannot allocate playback hw params\n");
		goto out;
	}
	if (snd_pcm_sw_params_malloc (&play_swpar) < 0) {
		fprintf (stderr, "cannot allocate playback sw params\n");
		goto out;
	}
	if (snd_pcm_hw_params_malloc (&capt_hwpar) < 0) {
		fprintf (stderr, "cannot allocate capture hw params\n");
		goto out;
	}
	if (snd_pcm_sw_params_malloc (&capt_swpar) < 0) {
		fprintf (stderr, "cannot allocate capture sw params\n");
		goto out;
	}

	io.synced = false;
	/* setup */
	if (io.play_handle) {
		if (set_hwpar (&io, play_hwpar, true) < 0) {
			goto out;
		}
		if (set_swpar (&io, play_swpar, true) < 0) {
			goto out;
		}
		io.play_npfd = snd_pcm_poll_descriptors_count (io.play_handle);
	}

	if (io.capt_handle) {
		if (set_hwpar (&io, capt_hwpar, false) < 0) {
			goto out;
		}
		if (set_swpar (&io, capt_swpar, false) < 0) {
			goto out;
		}
		io.capt_npfd = snd_pcm_poll_descriptors_count (io.capt_handle);

		if (io.play_handle && sync) {
			io.synced = ! snd_pcm_link (io.play_handle, io.capt_handle);
		}
	}

	/* verify settings */
	if (io.play_handle) {
		int dir;
		unsigned int val;
		snd_pcm_uframes_t fc;
		if (snd_pcm_hw_params_get_rate (play_hwpar, &val, &dir) || (val != io.samplerate) || dir) {
			fprintf (stderr, "cannot get requested sample rate for playback.\n");
			goto out;
		}
		if (snd_pcm_hw_params_get_period_size (play_hwpar, &fc, &dir) || (fc != io.samples_per_period) || dir) {
			fprintf (stderr, "cannot get requested period size for playback.\n");
			goto out;
		}
		if (snd_pcm_hw_params_get_periods (play_hwpar, &val, &dir) || (val != io.play_periods_per_cycle) || dir)
		{
			fprintf (stderr, "cannot get requested number of periods for playback.\n");
			goto out;
		}
	}

	if (io.capt_handle) {
		int dir;
		unsigned int val;
		snd_pcm_uframes_t fc;
		if (snd_pcm_hw_params_get_rate (capt_hwpar, &val, &dir) || (val != io.samplerate) || dir) {
			fprintf (stderr, "cannot get requested sample rate for capture.\n");
			goto out;
		}
		if (snd_pcm_hw_params_get_period_size (capt_hwpar, &fc, &dir) || (fc != io.samples_per_period) || dir) {
			fprintf (stderr, "cannot get requested period size for capture.\n");
			goto out;
		}
		if (snd_pcm_hw_params_get_periods (capt_hwpar, &val, &dir) || (val != io.capt_periods_per_cycle) || dir)
		{
			fprintf (stderr, "cannot get requested number of periods for capture.\n");
			goto out;
		}
	}

	if (io.play_handle) {
		snd_pcm_hw_params_get_format (play_hwpar, &play_format);
		snd_pcm_hw_params_get_access (play_hwpar, &play_access);

		switch (play_format) {
			case SND_PCM_FORMAT_FLOAT_LE:
			case SND_PCM_FORMAT_S32_LE:
			case SND_PCM_FORMAT_S32_BE:
			case SND_PCM_FORMAT_S24_LE:
			case SND_PCM_FORMAT_S24_BE:
				io.play_bytes_per_sample = 4;
				break;
			case SND_PCM_FORMAT_S24_3LE:
			case SND_PCM_FORMAT_S24_3BE:
				io.play_bytes_per_sample = 3;
				break;
			case SND_PCM_FORMAT_S16_LE:
			case SND_PCM_FORMAT_S16_BE:
				io.play_bytes_per_sample = 2;
				break;
			default:
				fprintf (stderr, "Cannot handle playback sample format.\n");
				goto out;
		}
	}

	if (io.capt_handle) {
		snd_pcm_hw_params_get_format (capt_hwpar, &capt_format);
		snd_pcm_hw_params_get_access (capt_hwpar, &capt_access);

		switch (capt_format) {
			case SND_PCM_FORMAT_FLOAT_LE:
			case SND_PCM_FORMAT_S32_LE:
			case SND_PCM_FORMAT_S32_BE:
			case SND_PCM_FORMAT_S24_LE:
			case SND_PCM_FORMAT_S24_BE:
				io.capt_bytes_per_sample = 4;
				break;
			case SND_PCM_FORMAT_S24_3LE:
			case SND_PCM_FORMAT_S24_3BE:
				io.capt_bytes_per_sample = 3;
				break;
			case SND_PCM_FORMAT_S16_LE:
			case SND_PCM_FORMAT_S16_BE:
				io.capt_bytes_per_sample = 2;
				break;
			default:
				fprintf (stderr, "Cannot handle capture sample format.\n");
				goto out;
		}
	}

	fprintf (stdout, "playback: ");
	if (io.play_handle) {
		fprintf (stdout, "\n");
		fprintf (stdout, "  channels   : %d\n",  io.play_nchan);
		fprintf (stdout, "  samplerate : %d\n",  io.samplerate);
		fprintf (stdout, "  buffersize : %ld\n", io.samples_per_period);
		fprintf (stdout, "  periods    : %d\n",  io.play_periods_per_cycle);
		fprintf (stdout, "  format     : %s\n",  snd_pcm_format_name (play_format));
	} else {
		fprintf (stdout, " not enabled\n");
		io.play_nchan = 0;
	}
	fprintf (stdout, "capture:  ");
	if (io.capt_handle) {
		fprintf (stdout, "\n");
		fprintf (stdout, "  channels   : %d\n",  io.capt_nchan);
		fprintf (stdout, "  samplerate : %d\n",  io.samplerate);
		fprintf (stdout, "  buffersize : %ld\n", io.samples_per_period);
		fprintf (stdout, "  periods    : %d\n",  io.capt_periods_per_cycle);
		fprintf (stdout, "  format     : %s\n",  snd_pcm_format_name (capt_format));
		if (io.play_handle) {
			fprintf (stdout, "%s\n", io.synced ? "synced" : "not synced");
		}
	} else {
		fprintf (stdout, " not enabled\n");
		io.capt_nchan = 0;
	}

	if (pcm_start (&io)) {
		goto out;
	}

	n_bufs = io.play_nchan > io.capt_nchan ? io.play_nchan : io.capt_nchan;
	io.testbuffers = (float**) malloc (n_bufs * sizeof (float*));

	for (i = 0; i < n_bufs; ++i) {
		io.testbuffers[i] = (float*) malloc (io.samples_per_period * sizeof (float));
	}

	signal (SIGINT, handle_sig);

	if (noop) {
		// only open the device, don't do anything
		if (io.run_for == 0) {
			while (!signalled) {
				sleep (1);
			}
		} else {
			sleep (io.run_for);
		}
	} else {
		if (rt_priority < 0) {
			err = realtime_pthread_create (SCHED_FIFO, rt_priority, 100000, &process_thread, run_thread, &io);
		} else {
			err = pthread_create (&process_thread, NULL, run_thread, &io);
		}

		if (err) {
			fprintf (stderr, "cannot create realtime process thread.\n");
			pcm_stop (&io);
			goto out;
		} else {
			void *status;
			pthread_join (process_thread, &status);
		}
	}

	if (pcm_stop (&io)) {
		goto out;
	}

	rv = 0;

out:
	free (play_device);
	free (capt_device);

	if (io.play_handle) {
		snd_pcm_close (io.play_handle);
	}
	if (io.capt_handle) {
		snd_pcm_close (io.capt_handle);
	}
	for (i = 0; i < n_bufs; i++) {
		free (io.testbuffers[i]);
	}
	free (io.testbuffers);

	snd_pcm_sw_params_free (capt_swpar);
	snd_pcm_hw_params_free (capt_hwpar);
	snd_pcm_sw_params_free (play_swpar);
	snd_pcm_hw_params_free (play_hwpar);

	return rv;
}
