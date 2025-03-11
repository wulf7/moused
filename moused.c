/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 1997-2000 Kazutaka YOKOTA <yokota@FreeBSD.org>
 * Copyright (c) 2004-2008 Philip Paeps <philip@FreeBSD.org>
 * Copyright (c) 2008 Jean-Sebastien Pedron <dumbbell@FreeBSD.org>
 * Copyright (c) 2021,2024 Vladimir Kondratyev <wulf@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * MOUSED.C
 *
 * Mouse daemon : listens to a evdev device node for mouse data stream,
 * interprets data and passes ioctls off to the console driver.
 *
 */

#include <sys/param.h>
#include <sys/bitstring.h>
#include <sys/consio.h>
#include <sys/mouse.h>
#include <sys/time.h>

#include <dev/evdev/input.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <math.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "util.h"
#include "quirks.h"

/*
 * bitstr_t implementation must be identical to one found in EVIOCG*
 * libevdev ioctls. Our bitstring(3) API is compatible since r299090.
 */
_Static_assert(sizeof(bitstr_t) == sizeof(unsigned long),
    "bitstr_t size mismatch");

#define MAX_CLICKTHRESHOLD	2000	/* 2 seconds */
#define MAX_BUTTON2TIMEOUT	2000	/* 2 seconds */
#define DFLT_CLICKTHRESHOLD	 500	/* 0.5 second */
#define DFLT_BUTTON2TIMEOUT	 100	/* 0.1 second */
#define DFLT_SCROLLTHRESHOLD	   3	/* 3 pixels */
#define DFLT_SCROLLSPEED	   2	/* 2 pixels */
#define	DFLT_MOUSE_RESOLUTION	   8	/* dpmm, == 200dpi */
#define	DFLT_TPAD_RESOLUTION	  40	/* dpmm, typical X res for Synaptics */
#define	DFLT_LINEHEIGHT		  10	/* pixels per line */

/* Abort 3-button emulation delay after this many movement events. */
#define BUTTON2_MAXMOVE	3

#define MOUSE_XAXIS	(-1)
#define MOUSE_YAXIS	(-2)

#define	ChordMiddle	0x0001
#define Emulate3Button	0x0002

#define	MAX_FINGERS	10

#define ID_NONE		0
#define ID_PORT		1
/* Was	ID_IF		2 */
#define ID_TYPE		4
#define ID_MODEL	8
#define ID_ALL		(ID_PORT | ID_TYPE | ID_MODEL)

/* Operations on timespecs */
#define	tsclr(tvp)		timespecclear(tvp)
#define	tscmp(tvp, uvp, cmp)	timespeccmp(tvp, uvp, cmp)
#define	tssub(tvp, uvp, vvp)	timespecsub(tvp, uvp, vvp)
#define	msec2ts(msec)	(struct timespec) {			\
	.tv_sec = (msec) / 1000,				\
	.tv_nsec = (msec) % 1000 * 1000000,			\
}
static inline struct timespec
tsaddms(struct timespec* tsp, u_int ms)
{
	struct timespec ret;

	ret = msec2ts(ms);
	timespecadd(tsp, &ret, &ret);

	return (ret);
};

static inline struct timespec
tssubms(struct timespec* tsp, u_int ms)
{
	struct timespec ret;

	ret = msec2ts(ms);
	timespecsub(tsp, &ret, &ret);

	return (ret);
};

#define debug(...) do {						\
	if (debug && nodaemon)					\
		warnx(__VA_ARGS__);				\
} while (0)

#define logerr(e, ...) do {					\
	log_or_warn(LOG_DAEMON | LOG_ERR, errno, __VA_ARGS__);	\
	exit(e);						\
} while (0)

#define logerrx(e, ...) do {					\
	log_or_warn(LOG_DAEMON | LOG_ERR, 0, __VA_ARGS__);	\
	exit(e);						\
} while (0)

#define logwarn(...)						\
	log_or_warn(LOG_DAEMON | LOG_WARNING, errno, __VA_ARGS__)

#define logwarnx(...)						\
	log_or_warn(LOG_DAEMON | LOG_WARNING, 0, __VA_ARGS__)

enum gesture {
	GEST_IGNORE,
	GEST_ACCUMULATE,
	GEST_MOVE,
	GEST_VSCROLL,
	GEST_HSCROLL,
};

/* structures */

/* global variables */

static int	debug = 0;
static bool	nodaemon = false;
static bool	background = false;
static bool	paused = false;
static bool	grab = false;
static int	identify = ID_NONE;
static const char *devpath = NULL;
static const char *pidfile = "/var/run/moused.pid";
static struct pidfh *pfh;
static const char *config_file = CONFSDIR "/moused.conf";
static const char *quirks_path = QUIRKSDIR;
static struct quirks_context *quirks;

static bool	opt_drift_terminate = false;
static u_int	opt_drift_distance = 4;		/* max steps X+Y */
static u_int	opt_drift_time = 500;		/* ms */
static u_int	opt_drift_after = 4000;		/* ms */

static double	opt_accelx = 1.0;
static double	opt_accely = 1.0;
static bool	opt_exp_accel = false;
static double	opt_expoaccel = 1.0;
static double	opt_expoffset = 1.0;

static bool	opt_virtual_scroll = false;
static bool	opt_hvirtual_scroll = false;

/* local variables */

/* types (the table must be ordered by DEVICE_TYPE_XXX in util.h) */
static const char *rnames[] = {
	[DEVICE_TYPE_MOUSE]		= "mouse",
	[DEVICE_TYPE_POINTINGSTICK]	= "pointing stick",
	[DEVICE_TYPE_TOUCHPAD]		= "touchpad",
	[DEVICE_TYPE_TOUCHSCREEN]	= "touchscreen",
	[DEVICE_TYPE_TABLET]		= "tablet",
	[DEVICE_TYPE_TABLET_PAD]	= "tablet pad",
	[DEVICE_TYPE_KEYBOARD]		= "keyboard",
	[DEVICE_TYPE_JOYSTICK]		= "joystick",
};

struct tpcaps {
	bool	is_clickpad;
	bool	is_mt;
	bool	cap_touch;
	bool	cap_pressure;
	bool	cap_width;
	int	min_x;
	int	max_x;
	int	min_y;
	int	max_y;
	int	res_x;	/* dots per mm */
	int	res_y;	/* dots per mm */
};

struct tpinfo {
	bool	two_finger_scroll;	/* Enable two finger scrolling */
	bool	natural_scroll;		/* Enable natural scrolling */
	bool	three_finger_drag;	/* Enable dragging with three fingers */
	u_int	min_pressure_hi;	/* Min pressure to start an action */
	u_int	min_pressure_lo;	/* Min pressure to continue an action */
	u_int	max_pressure;		/* Maximum pressure to detect palm */
	u_int	max_width;		/* Max finger width to detect palm */
	int	margin_top;		/* Top margin */
	int	margin_right;		/* Right margin */
	int	margin_bottom;		/* Bottom margin */
	int	margin_left;		/* Left margin */
	u_int	tap_timeout;		/* */
	u_int	tap_threshold;		/* Minimum pressure to detect a tap */
	double	tap_max_delta;		/* Length of segments above which a tap is ignored */
	u_int	taphold_timeout;	/* Maximum elapsed time between two taps to consider a tap-hold action */
	double	vscroll_ver_area;	/* Area reserved for vertical virtual scrolling */
	double	vscroll_hor_area;	/* Area reserved for horizontal virtual scrolling */
	double	vscroll_min_delta;	/* Minimum movement to consider virtual scrolling */
	int	softbuttons_y;		/* Vertical size of softbuttons area */
	int	softbutton2_x;		/* Horizontal offset of 2-nd softbutton left edge */
	int	softbutton3_x;		/* Horizontal offset of 3-rd softbutton left edge */
};

static struct gesture_state {
	int 		start_x;
	int 		start_y;
	int 		prev_x;
	int 		prev_y;
	int		prev_nfingers;
	int		fingers_nb;
	int		tap_button;
	bool		fingerdown;
	bool		in_taphold;
	int		in_vscroll;
	int		zmax;           /* maximum pressure value */
	struct timespec	taptimeout;     /* tap timeout for touchpads */
	struct timespec	startdelay;
	int		idletimeout;
} gesture = {
	.idletimeout = -1,
};

struct finger {
	int	x;
	int	y;
	int	p;
	int	w;
	int	id;	/* id=0 - no touch, id>1 - touch id */
};

static struct evdev_state {
	int		buttons;
	/* Relative */
	int		dx;
	int		dy;
	int		dz;
	int		dw;
	int		acc_dx;
	int		acc_dy;
	/* Absolute single-touch */
	int		nfingers;
	struct finger	st;
	/* Absolute multi-touch */
	int		slot;
	struct finger	mt[MAX_FINGERS];
} ev;

enum scroll_state {
	SCROLL_NOTSCROLLING,
	SCROLL_PREPARE,
	SCROLL_SCROLLING,
};

struct scroll {
	int	threshold;	/* Movement distance before virtual scrolling */
	int	speed;		/* Movement distance to rate of scrolling */
	enum scroll_state state;
	int	movement;
	int	hmovement;
};

struct drift_xy {
	int x;
	int y;
};
struct drift {
	u_int		distance;	/* max steps X+Y */
	u_int		time;		/* ms */
	struct timespec	time_ts;
	struct timespec	twotime_ts;	/* 2*drift_time */
	u_int		after;		/* ms */
	struct timespec	after_ts;
	bool		terminate;
	struct timespec	current_ts;
	struct timespec	last_activity;
	struct timespec	since;
	struct drift_xy	last;		/* steps in last drift_time */
	struct drift_xy	previous;	/* steps in prev. drift_time */
};

struct accel {
	bool is_exponential;	/* Exponential acceleration is enabled */
	double accelx;		/* Acceleration in the X axis */
	double accely;		/* Acceleration in the Y axis */
	double accelz;		/* Acceleration in the wheel axis */
	double expoaccel;	/* Exponential acceleration */
	double expoffset;	/* Movement offset for exponential accel. */
	double remainx;		/* Remainder on X, Y and wheel axis, ... */
	double remainy;		/*    ...  respectively to compensate */
	double remainz;		/*    ... for rounding errors. */
	double lastlength[3];
};

static struct rodentparam {
	int flags;
	struct device dev;	/* Device */
	struct quirks *quirks;	/* Configuration file and quirks */
	int zmap[4];		/* MOUSE_{X|Y}AXIS or a button number */
	int wmode;		/* wheel mode button number */
	int mfd;		/* mouse file descriptor */
	int cfd;		/* /dev/consolectl file descriptor */
	long clickthreshold;	/* double click speed in msec */
	long button2timeout;	/* 3 button emulation timeout */
	struct drift drift;
	struct accel accel;	/* cursor acceleration state */
	struct scroll scroll;	/* virtual scroll state */
	struct tpcaps tphw;	/* touchpad capabilities */
	struct tpinfo tpinfo;	/* touchpad gesture parameters */
} rodent = {
	.flags = 0,
	.dev.path = NULL,
	.dev.type = DEVICE_TYPE_UNKNOWN,
	.quirks = NULL,
	.zmap = { 0, 0, 0, 0 },
	.wmode = 0,
	.mfd = -1,
	.cfd = -1,
	.clickthreshold = DFLT_CLICKTHRESHOLD,
	.button2timeout = DFLT_BUTTON2TIMEOUT,
	.scroll = {
		.threshold = DFLT_SCROLLTHRESHOLD,
		.speed = DFLT_SCROLLSPEED,
	},
	.tpinfo = {
		.two_finger_scroll = true,
		.natural_scroll = false,
		.three_finger_drag = false,
		.min_pressure_hi = 1,
		.min_pressure_lo = 1,
		.max_pressure = 130,
		.max_width = 16,
		.tap_timeout = 180,		/* ms */
		.tap_threshold = 0,
		.tap_max_delta = 1.3,		/* mm */
		.taphold_timeout = 300,		/* ms */
		.vscroll_min_delta = 1.25,	/* mm */
		.vscroll_hor_area = 0.0,	/* mm */
		.vscroll_ver_area = -15.0,	/* mm */
	},
};

/* button status */
struct button_state {
	int count;	/* 0: up, 1: single click, 2: double click,... */
	struct timespec ts;	/* timestamp on the last button event */
};
static struct button_state	bstate[MOUSE_MAXBUTTON];	/* button state */
static struct button_state	*mstate[MOUSE_MAXBUTTON];/* mapped button st.*/
static struct button_state	zstate[4];		 /* Z/W axis state */

/* state machine for 3 button emulation */

enum bt3_emul_state {
	S0,		/* start */
	S1,		/* button 1 delayed down */
	S2,		/* button 3 delayed down */
	S3,		/* both buttons down -> button 2 down */
	S4,		/* button 1 delayed up */
	S5,		/* button 1 down */
	S6,		/* button 3 down */
	S7,		/* both buttons down */
	S8,		/* button 3 delayed up */
	S9,		/* button 1 or 3 up after S3 */
};

#define A(b1, b3)	(((b1) ? 2 : 0) | ((b3) ? 1 : 0))
#define A_TIMEOUT	4
#define S_DELAYED(st)	(states[st].s[A_TIMEOUT] != (st))

static const struct {
	enum bt3_emul_state s[A_TIMEOUT + 1];
	int buttons;
	int mask;
	bool timeout;
} states[10] = {
    /* S0 */
    { { S0, S2, S1, S3, S0 }, 0, ~(MOUSE_BUTTON1DOWN | MOUSE_BUTTON3DOWN), false },
    /* S1 */
    { { S4, S2, S1, S3, S5 }, 0, ~MOUSE_BUTTON1DOWN, false },
    /* S2 */
    { { S8, S2, S1, S3, S6 }, 0, ~MOUSE_BUTTON3DOWN, false },
    /* S3 */
    { { S0, S9, S9, S3, S3 }, MOUSE_BUTTON2DOWN, ~0, false },
    /* S4 */
    { { S0, S2, S1, S3, S0 }, MOUSE_BUTTON1DOWN, ~0, true },
    /* S5 */
    { { S0, S2, S5, S7, S5 }, MOUSE_BUTTON1DOWN, ~0, false },
    /* S6 */
    { { S0, S6, S1, S7, S6 }, MOUSE_BUTTON3DOWN, ~0, false },
    /* S7 */
    { { S0, S6, S5, S7, S7 }, MOUSE_BUTTON1DOWN | MOUSE_BUTTON3DOWN, ~0, false },
    /* S8 */
    { { S0, S2, S1, S3, S0 }, MOUSE_BUTTON3DOWN, ~0, true },
    /* S9 */
    { { S0, S9, S9, S3, S9 }, 0, ~(MOUSE_BUTTON1DOWN | MOUSE_BUTTON3DOWN), false },
};
static enum bt3_emul_state	mouse_button_state;
static struct timespec	mouse_button_state_ts;
static int		mouse_move_delayed;

static jmp_buf env;

/* function prototypes */

static moused_log_handler	log_or_warn_va;

static void	linacc(int, int, int, int*, int*, int*);
static void	expoacc(int, int, int, int*, int*, int*);
static void	moused(void);
static void	reset(int sig);
static void	pause_mouse(int sig);
static void	usage(void);
static void	log_or_warn(int log_pri, int errnum, const char *fmt, ...)
		    __printflike(3, 4);

static enum device_type	r_identify(int fd);
static const char *r_name(int type);
static int	r_init(const char *path);
static int	r_protocol(struct input_event *b, mousestatus_t *act);
static void	r_vscroll_detect(mousestatus_t *act);
static void	r_vscroll(mousestatus_t *act);
static int	r_statetrans(mousestatus_t *a1, mousestatus_t *a2, int trans);
static bool	r_installmap(char *arg);
static void	r_map(mousestatus_t *act1, mousestatus_t *act2);
static void	r_timestamp(mousestatus_t *act);
static bool	r_timeout(void);
static void	r_click(mousestatus_t *act);
static bool	r_drift(struct drift *, mousestatus_t *);
static enum gesture r_gestures(int x0, int y0, int z, int w, int nfingers,
		    struct timespec *time, mousestatus_t *ms);

int
main(int argc, char *argv[])
{
	pid_t mpid;
	int c;
	int	i;
	int	j;

	for (i = 0; i < MOUSE_MAXBUTTON; ++i)
		mstate[i] = &bstate[i];

	while ((c = getopt(argc, argv, "3A:C:E:HI:L:T:VU:a:cdfghi:m:p:q:w:z:"))
	    != -1) {
		switch(c) {

		case '3':
			rodent.flags |= Emulate3Button;
			break;

		case 'E':
			rodent.button2timeout = atoi(optarg);
			if ((rodent.button2timeout < 0) ||
			    (rodent.button2timeout > MAX_BUTTON2TIMEOUT)) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			break;

		case 'a':
			i = sscanf(optarg, "%lf,%lf", &opt_accelx, &opt_accely);
			if (i == 0) {
				warnx("invalid linear acceleration argument "
				    "'%s'", optarg);
				usage();
			}
			if (i == 1)
				opt_accely = opt_accelx;
			break;

		case 'A':
			opt_exp_accel = true;
			i = sscanf(optarg, "%lf,%lf", &opt_expoaccel,
			    &opt_expoffset);
			if (i == 0) {
				warnx("invalid exponential acceleration "
				    "argument '%s'", optarg);
				usage();
			}
			if (i == 1)
				opt_expoffset = 1.0;
			break;

		case 'c':
			rodent.flags |= ChordMiddle;
			break;

		case 'd':
			++debug;
			break;

		case 'f':
			nodaemon = true;
			break;

		case 'g':
			grab = true;
			break;

		case 'i':
			if (strcmp(optarg, "all") == 0)
				identify = ID_ALL;
			else if (strcmp(optarg, "port") == 0)
				identify = ID_PORT;
			else if (strcmp(optarg, "type") == 0)
				identify = ID_TYPE;
			else if (strcmp(optarg, "model") == 0)
				identify = ID_MODEL;
			else {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			nodaemon = true;
			break;

		case 'm':
			if (!r_installmap(optarg)) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			break;

		case 'p':
			devpath = optarg;
			break;

		case 'w':
			i = atoi(optarg);
			if ((i <= 0) || (i > MOUSE_MAXBUTTON)) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			rodent.wmode = 1 << (i - 1);
			break;

		case 'z':
			if (strcmp(optarg, "x") == 0) {
				rodent.zmap[0] = MOUSE_XAXIS;
				break;
			}
			if (strcmp(optarg, "y") == 0) {
				rodent.zmap[0] = MOUSE_YAXIS;
				break;
			}
			i = atoi(optarg);
			/*
			 * Use button i for negative Z axis movement and
			 * button (i + 1) for positive Z axis movement.
			 */
			if ((i <= 0) || (i > MOUSE_MAXBUTTON - 1)) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			rodent.zmap[0] = i;
			rodent.zmap[1] = i + 1;
			debug("optind: %d, optarg: '%s'", optind, optarg);
			for (j = 1; j < 4; ++j) {
				if ((optind >= argc) || !isdigit(*argv[optind]))
					break;
				i = atoi(argv[optind]);
				if ((i <= 0) || (i > MOUSE_MAXBUTTON - 1)) {
					warnx("invalid argument `%s'",
					    argv[optind]);
					usage();
				}
				rodent.zmap[j] = i;
				++optind;
			}
			if ((rodent.zmap[2] != 0) && (rodent.zmap[3] == 0))
				rodent.zmap[3] = rodent.zmap[2] + 1;
			break;

		case 'C':
			rodent.clickthreshold = atoi(optarg);
			if ((rodent.clickthreshold < 0) ||
			    (rodent.clickthreshold > MAX_CLICKTHRESHOLD)) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			break;

		case 'H':
			opt_hvirtual_scroll = true;
			break;
		
		case 'I':
			pidfile = optarg;
			break;

		case 'L':
			rodent.scroll.speed = atoi(optarg);
			if (rodent.scroll.speed < 0) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			break;

		case 'q':
			config_file = optarg;
			break;

		case 'Q':
			quirks_path = optarg;
			break;

		case 'T':
			opt_drift_terminate = true;
			sscanf(optarg, "%u,%u,%u", &opt_drift_distance,
			    &opt_drift_time, &opt_drift_after);
			if (opt_drift_distance == 0 ||
			    opt_drift_time == 0 ||
			    opt_drift_after == 0) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			break;

		case 'V':
			opt_virtual_scroll = true;
			break;

		case 'U':
			rodent.scroll.threshold = atoi(optarg);
			if (rodent.scroll.threshold < 0) {
				warnx("invalid argument `%s'", optarg);
				usage();
			}
			break;

		case 'h':
		case '?':
		default:
			usage();
		}
	}

	if (devpath == NULL) {
		warnx("no port name specified");
		usage();
	}

	if ((rodent.cfd = open("/dev/consolectl", O_RDWR, 0)) == -1)
		logerr(1, "cannot open /dev/consolectl");

	switch (setjmp(env)) {
	case SIGHUP:
		quirks_context_unref(quirks);
		close(rodent.mfd);
		/* FALLTHROUGH */
	case 0:
		break;
	case SIGINT:
	case SIGQUIT:
	case SIGTERM:
		exit(0);
		/* NOT REACHED */
	default:
		goto out;
	}

	signal(SIGHUP , reset);
	signal(SIGINT , reset);
	signal(SIGQUIT, reset);
	signal(SIGTERM, reset);
	signal(SIGUSR1, pause_mouse);

	quirks = quirks_init_subsystem(quirks_path, config_file,
	    log_or_warn_va,
	    background ? QLOG_MOUSED_LOGGING : QLOG_CUSTOM_LOG_PRIORITIES);
	if (quirks == NULL)
		logwarnx("cannot open configuration file %s", config_file);

	if ((rodent.mfd = r_init(devpath)) < 0)
		logerrx(1, "Can not initialize device");

	/* print some information */
	if (identify != ID_NONE) {
		if (identify == ID_ALL)
			printf("%s %s %s\n", rodent.dev.path,
			    r_name(rodent.dev.type), rodent.dev.name);
		else if (identify & ID_PORT)
			printf("%s\n", rodent.dev.path);
		else if (identify & ID_TYPE)
			printf("%s\n", r_name(rodent.dev.type));
		else if (identify & ID_MODEL)
			printf("%s\n", rodent.dev.name);
		exit(0);
	}

	if (!nodaemon && !background) {
		pfh = pidfile_open(pidfile, 0600, &mpid);
		if (pfh == NULL) {
			if (errno == EEXIST)
				logerrx(1, "moused already running, pid: %d", mpid);
			logwarn("cannot open pid file");
		}
		if (daemon(0, 0)) {
			int saved_errno = errno;
			pidfile_remove(pfh);
			errno = saved_errno;
			logerr(1, "failed to become a daemon");
		} else {
			background = true;
			pidfile_write(pfh);
		}
	}

	moused();

out:
	quirks_context_unref(quirks);

	if (rodent.mfd != -1)
		close(rodent.mfd);
	if (rodent.cfd != -1)
		close(rodent.cfd);

	exit(0);
}

/*
 * Function to calculate linear acceleration.
 *
 * If there are any rounding errors, the remainder
 * is stored in the remainx and remainy variables
 * and taken into account upon the next movement.
 */

static void
linacc(int dx, int dy, int dz, int *movex, int *movey, int *movez)
{
	struct accel *acc = &rodent.accel;
	double fdx, fdy, fdz;

	if (dx == 0 && dy == 0 && dz == 0) {
		*movex = *movey = *movez = 0;
		return;
	}
	fdx = dx * acc->accelx + acc->remainx;
	fdy = dy * acc->accely + acc->remainy;
	fdz = dz * acc->accelz + acc->remainz;
	*movex = lround(fdx);
	*movey = lround(fdy);
	*movez = lround(fdz);
	acc->remainx = fdx - *movex;
	acc->remainy = fdy - *movey;
	acc->remainz = fdz - *movez;
}

/*
 * Function to calculate exponential acceleration.
 * (Also includes linear acceleration if enabled.)
 *
 * In order to give a smoother behaviour, we record the four
 * most recent non-zero movements and use their average value
 * to calculate the acceleration.
 */

static void
expoacc(int dx, int dy, int dz, int *movex, int *movey, int *movez)
{
	struct accel *acc = &rodent.accel;
	double fdx, fdy, fdz, length, lbase, accel;

	if (dx == 0 && dy == 0 && dz == 0) {
		*movex = *movey = *movez = 0;
		return;
	}
	fdx = dx * acc->accelx;
	fdy = dy * acc->accely;
	fdz = dz * acc->accelz;
	length = sqrt((fdx * fdx) + (fdy * fdy));	/* Pythagoras */
	length = (length + acc->lastlength[0] + acc->lastlength[1] +
	    acc->lastlength[2]) / 4;
	lbase = length / acc->expoffset;
	accel = pow(lbase, acc->expoaccel) / lbase;
	fdx = fdx * accel + acc->remainx;
	fdy = fdy * accel + acc->remainy;
	*movex = lround(fdx);
	*movey = lround(fdy);
	*movez = lround(fdz);
	acc->remainx = fdx - *movex;
	acc->remainy = fdy - *movey;
	acc->remainz = fdz - *movez;
	acc->lastlength[2] = acc->lastlength[1];
	acc->lastlength[1] = acc->lastlength[0];
	/* Insert new average, not original length! */
	acc->lastlength[0] = length;
}

static void
moused(void)
{
    struct mouse_info mouse;
    mousestatus_t action0;		/* original mouse action */
    mousestatus_t action;		/* interim buffer */
    mousestatus_t action2;		/* mapped action */
    int timeout;
    bool timeout_em3b;
    struct pollfd fds;
    struct input_event b;
    int flags;
    int c;
    int i;

    /* clear mouse data */
    bzero(&action0, sizeof(action0));
    bzero(&action, sizeof(action));
    bzero(&action2, sizeof(action2));
    bzero(&mouse, sizeof(mouse));
    /* process mouse data */
    for (;;) {

	fds.fd = rodent.mfd;
	fds.events = POLLIN;
	fds.revents = 0;
	timeout = -1;
	if ((rodent.flags & Emulate3Button) && S_DELAYED(mouse_button_state)) {
	    timeout = 20;
	    timeout_em3b = true;
	}
	if (gesture.idletimeout != -1) {
	    if (timeout == -1 || gesture.idletimeout < timeout) {
		timeout = gesture.idletimeout;
		timeout_em3b = false;
	    } else
		gesture.idletimeout -= timeout;
	}

	c = poll(&fds, 1, timeout);
	if (c < 0) {                    /* error */
	    logwarn("failed to read from mouse");
	    continue;
	} else if (c == 0 && timeout_em3b) {	/* timeout */
	    /* assert(rodent.flags & Emulate3Button) */
	    action0.button = action0.obutton;
	    action0.dx = action0.dy = action0.dz = 0;
	    action0.flags = flags = 0;
	    if (r_timeout() && r_statetrans(&action0, &action, A_TIMEOUT)) {
		if (debug > 2)
		    debug("flags:%08x buttons:%08x obuttons:%08x",
			  action.flags, action.button, action.obutton);
	    } else {
		action0.obutton = action0.button;
		continue;
	    }
	} else {
	    /* mouse movement */
	    if (c > 0) {
		if ((fds.revents & POLLIN) == 0)
		    return;
		if (read(rodent.mfd, &b, sizeof(b)) == -1) {
		    if (errno == EWOULDBLOCK)
			continue;
		    else
			return;
		}
	    } else {
		b.time.tv_sec = timeout == 0 ? 0 : LONG_MAX;
		b.time.tv_usec = 0;
		b.type = EV_SYN;
		b.code = SYN_REPORT;
		b.value = 1;
	    }
	    gesture.idletimeout = -1;
	    if ((flags = r_protocol(&b, &action0)) == 0)
		continue;

	    if (opt_virtual_scroll || opt_hvirtual_scroll) {
		if (action0.button == MOUSE_BUTTON2DOWN) {
			debug("[BUTTON2] flags:%08x buttons:%08x obuttons:%08x",
			    action.flags, action.button, action.obutton);
		} else {
			debug("[NOTBUTTON2] flags:%08x buttons:%08x obuttons:%08x",
			    action.flags, action.button, action.obutton);
		}
		r_vscroll_detect(&action0);
	    }

	    r_timestamp(&action0);
	    r_statetrans(&action0, &action,
			 A(action0.button & MOUSE_BUTTON1DOWN,
			   action0.button & MOUSE_BUTTON3DOWN));
	    debug("flags:%08x buttons:%08x obuttons:%08x", action.flags,
		  action.button, action.obutton);
	}
	action0.obutton = action0.button;
	flags &= MOUSE_POSCHANGED;
	flags |= action.obutton ^ action.button;
	action.flags = flags;

	if (flags) {			/* handler detected action */
	    r_map(&action, &action2);
	    debug("activity : buttons 0x%08x  dx %d  dy %d  dz %d",
		action2.button, action2.dx, action2.dy, action2.dz);

	    if (opt_virtual_scroll || opt_hvirtual_scroll) {
		/*
		 * If *only* the middle button is pressed AND we are moving
		 * the stick/trackpoint/nipple, scroll!
		 */
		r_vscroll(&action2);
	    }

		if (rodent.drift.terminate) {
			if ((flags & MOUSE_POSCHANGED) == 0 ||
			    action.dz || action2.dz)
				rodent.drift.last_activity =
				    rodent.drift.current_ts;
			else {
				if (r_drift (&rodent.drift, &action2))
					continue;
			}
		}

		/* Defer clicks until we aren't VirtualScroll'ing. */
		if (rodent.scroll.state == SCROLL_NOTSCROLLING)
			r_click(&action2);

		if (action2.flags & MOUSE_POSCHANGED) {
			mouse.operation = MOUSE_MOTION_EVENT;
			mouse.u.data.buttons = action2.button;
			if (rodent.accel.is_exponential) {
				expoacc(action2.dx, action2.dy, action2.dz,
				    &mouse.u.data.x, &mouse.u.data.y, &mouse.u.data.z);
			} else {
				linacc(action2.dx, action2.dy, action2.dz,
				    &mouse.u.data.x, &mouse.u.data.y, &mouse.u.data.z);
			}
			if (debug < 2 && !paused)
				ioctl(rodent.cfd, CONS_MOUSECTL, &mouse);
		}

	    /*
	     * If the Z axis movement is mapped to an imaginary physical
	     * button, we need to cook up a corresponding button `up' event
	     * after sending a button `down' event.
	     */
	    if ((rodent.zmap[0] > 0) && (action.dz != 0)) {
		action.obutton = action.button;
		action.dx = action.dy = action.dz = 0;
		r_map(&action, &action2);
		debug("activity : buttons 0x%08x  dx %d  dy %d  dz %d",
		    action2.button, action2.dx, action2.dy, action2.dz);

		r_click(&action2);
	    }
	}
    }
    /* NOT REACHED */
}

static void
reset(int sig)
{
	longjmp(env, sig);
}

static void
pause_mouse(__unused int sig)
{
	paused = !paused;
}

/*
 * usage
 *
 * Complain, and free the CPU for more worthy tasks
 */
static void
usage(void)
{
    fprintf(stderr, "%s\n%s\n%s\n%s\n%s\n",
	"usage: moused [-cdfg] [-I file]",
	"              [-VH [-U threshold]] [-a X[,Y]] [-C threshold] [-m N=M] [-w N]",
	"              [-z N] [-3 [-E timeout]]",
	"              [-T distance[,time[,after]]] -p <port> [-q config] [-Q quirks]",
	"       moused [-d] -i <port|type|model|all> -p <port>");
    exit(1);
}

/*
 * Output an error message to syslog or stderr as appropriate. If
 * `errnum' is non-zero, append its string form to the message.
 */
static void
log_or_warn_va(int log_pri, int errnum, const char *fmt, va_list ap)
{
	char buf[256];
	size_t len;

	if (debug == 0 && log_pri > LOG_ERR)
		return;

	vsnprintf(buf, sizeof(buf), fmt, ap);

	/* Strip trailing line-feed appended by quirk subsystem */
	len = strlen(buf);
	if (len != 0 && buf[len - 1] == '\n')
		buf[len - 1] = '\0';

	if (errnum) {
		strlcat(buf, ": ", sizeof(buf));
		strlcat(buf, strerror(errnum), sizeof(buf));
	}

	if (background)
		syslog(log_pri, "%s", buf);
	else
		warnx("%s", buf);
}

static void
log_or_warn(int log_pri, int errnum, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	log_or_warn_va(log_pri, errnum, fmt, ap);
	va_end(ap);
}

static inline int
bit_find(bitstr_t *array, int start, int stop)
{
	int res;

	bit_ffs_at(array, start, stop + 1, &res);
	return (res != -1);
}

/* Derived from EvdevProbe() function of xf86-input-evdev driver */
static enum device_type
r_identify(int fd)
{
	enum device_type type;
	bitstr_t bit_decl(key_bits, KEY_CNT); /* */
	bitstr_t bit_decl(rel_bits, REL_CNT); /* Evdev capabilities */
	bitstr_t bit_decl(abs_bits, ABS_CNT); /* */
	bitstr_t bit_decl(prop_bits, INPUT_PROP_CNT);
	bool has_keys, has_buttons, has_lmr, has_rel_axes, has_abs_axes;
	bool has_mt;

	/* maybe this is a evdev mouse... */
	if (ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel_bits)), rel_bits) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0 ||
	    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0 ||
	    ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits) < 0) {
		return (DEVICE_TYPE_UNKNOWN);
	}

	has_keys = bit_find(key_bits, 0, BTN_MISC - 1);
	has_buttons = bit_find(key_bits, BTN_MISC, BTN_JOYSTICK - 1);
	has_lmr = bit_find(key_bits, BTN_LEFT, BTN_MIDDLE);
	has_rel_axes = bit_find(rel_bits, 0, REL_MAX);
	has_abs_axes = bit_find(abs_bits, 0, ABS_MAX);
	has_mt = bit_find(abs_bits, ABS_MT_SLOT, ABS_MAX);
	type = DEVICE_TYPE_UNKNOWN;

	if (has_abs_axes) {
		if (has_mt && !has_buttons) {
			/* TBD:Improve joystick detection */
			if (bit_test(key_bits, BTN_JOYSTICK)) {
				return (DEVICE_TYPE_JOYSTICK);
			} else {
				has_buttons = true;
			}
		}

		if (bit_test(abs_bits, ABS_X) &&
		    bit_test(abs_bits, ABS_Y)) {
			if (bit_test(key_bits, BTN_TOOL_PEN) ||
			    bit_test(key_bits, BTN_STYLUS) ||
			    bit_test(key_bits, BTN_STYLUS2)) {
				type = DEVICE_TYPE_TABLET;
			} else if (bit_test(abs_bits, ABS_PRESSURE) ||
				   bit_test(key_bits, BTN_TOUCH)) {
				if (has_lmr ||
				    bit_test(key_bits, BTN_TOOL_FINGER)) {
					type = DEVICE_TYPE_TOUCHPAD;
				} else {
					type = DEVICE_TYPE_TOUCHSCREEN;
				}
			/* some touchscreens use BTN_LEFT rather than BTN_TOUCH */
			} else if (!(bit_test(rel_bits, REL_X) &&
				     bit_test(rel_bits, REL_Y)) &&
				     has_lmr) {
				type = DEVICE_TYPE_TOUCHSCREEN;
			}
		}
	}

	if (type == DEVICE_TYPE_UNKNOWN) {
		if (has_keys)
			type = DEVICE_TYPE_KEYBOARD;
		else if (has_rel_axes || has_buttons)
			type = DEVICE_TYPE_MOUSE;
	}

	return (type);
}

static const char *
r_name(int type)
{
	const char *unknown = "unknown";

	return (type == DEVICE_TYPE_UNKNOWN ||
	    type >= (int)(sizeof(rnames) / sizeof(rnames[0])) ?
	    unknown : rnames[type]);
}

static void
r_init_buttons(void)
{
	int i, j;

	/* fix Z axis mapping */
	for (i = 0; i < 4; ++i) {
		if (rodent.zmap[i] > 0) {
			for (j = 0; j < MOUSE_MAXBUTTON; ++j) {
				if (mstate[j] == &bstate[rodent.zmap[i] - 1])
					mstate[j] = &zstate[i];
			}
			rodent.zmap[i] = 1 << (rodent.zmap[i] - 1);
		}
	}

	mouse_button_state = S0;
	clock_gettime(CLOCK_MONOTONIC_FAST, &mouse_button_state_ts);
	mouse_move_delayed = 0;
	for (i = 0; i < MOUSE_MAXBUTTON; ++i) {
		bstate[i].count = 0;
		bstate[i].ts = mouse_button_state_ts;
	}
	for (i = 0; i < (int)(sizeof(zstate) / sizeof(zstate[0])); ++i) {
		zstate[i].count = 0;
		zstate[i].ts = mouse_button_state_ts;
	}
}

static void
r_init_touchpad(void)
{
	struct input_absinfo ai;
	bitstr_t bit_decl(key_bits, KEY_CNT);
	bitstr_t bit_decl(abs_bits, ABS_CNT);
	bitstr_t bit_decl(prop_bits, INPUT_PROP_CNT);
	struct tpcaps *tphw = &rodent.tphw;
	struct tpinfo *tpinfo = &rodent.tpinfo;
	struct quirks *q = rodent.quirks;
	struct quirk_range r;
	struct quirk_dimensions dim;
	int i;
	u_int u;
	int sz_x, sz_y;

	quirks_get_bool(q, MOUSED_TWO_FINGER_SCROLL, &tpinfo->two_finger_scroll);
	quirks_get_bool(q, MOUSED_NATURAL_SCROLL, &tpinfo->natural_scroll);
	quirks_get_bool(q, MOUSED_THREE_FINGER_DRAG, &tpinfo->three_finger_drag);
	quirks_get_uint32(q, MOUSED_TAP_TIMEOUT, &tpinfo->tap_timeout);
	quirks_get_double(q, MOUSED_TAP_MAX_DELTA, &tpinfo->tap_max_delta);
	quirks_get_uint32(q, MOUSED_TAPHOLD_TIMEOUT, &tpinfo->taphold_timeout);
	quirks_get_double(q, MOUSED_VSCROLL_MIN_DELTA, &tpinfo->vscroll_min_delta);
	quirks_get_double(q, MOUSED_VSCROLL_HOR_AREA, &tpinfo->vscroll_hor_area);
	quirks_get_double(q, MOUSED_VSCROLL_VER_AREA, &tpinfo->vscroll_ver_area);

	ioctl(rodent.mfd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits);
	ioctl(rodent.mfd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits);

	if (ioctl(rodent.mfd, EVIOCGABS(ABS_X), &ai) >= 0) {
		tphw->min_x = (ai.maximum > ai.minimum) ? ai.minimum : INT_MIN;
		tphw->max_x = (ai.maximum > ai.minimum) ? ai.maximum : INT_MAX;
		sz_x = (ai.maximum > ai.minimum) ? ai.maximum - ai.minimum : 0;
		tphw->res_x = ai.resolution == 0 ?
		    DFLT_TPAD_RESOLUTION : ai.resolution;
	}
	if (ioctl(rodent.mfd, EVIOCGABS(ABS_Y), &ai) >= 0) {
		tphw->min_y = (ai.maximum > ai.minimum) ? ai.minimum : INT_MIN;
		tphw->max_y = (ai.maximum > ai.minimum) ? ai.maximum : INT_MAX;
		sz_y = (ai.maximum > ai.minimum) ? ai.maximum - ai.minimum : 0;
		tphw->res_y = ai.resolution == 0 ?
		    DFLT_TPAD_RESOLUTION : ai.resolution;
	}
	if (quirks_get_dimensions(q, QUIRK_ATTR_RESOLUTION_HINT, &dim)) {
		tphw->res_x = dim.x;
		tphw->res_y = dim.y;
	} else if (tphw->max_x != INT_MAX && tphw->max_y != INT_MAX &&
		   quirks_get_dimensions(q, QUIRK_ATTR_SIZE_HINT, &dim)) {
		tphw->res_x = (tphw->max_x - tphw->min_x) / dim.x;
		tphw->res_y = (tphw->max_y - tphw->min_y) / dim.y;
	}
	if (bit_test(key_bits, BTN_TOUCH))
		tphw->cap_touch = true;
	/* XXX: libinput uses ABS_MT_PRESSURE where available */
	if (bit_test(abs_bits, ABS_PRESSURE) &&
	    ioctl(rodent.mfd, EVIOCGABS(ABS_PRESSURE), &ai) >= 0) {
		tphw->cap_pressure = true;
		if (quirks_get_range(q, QUIRK_ATTR_PRESSURE_RANGE, &r)) {
			if (r.upper == 0 && r.lower == 0) {
				debug("pressure-based touch detection disabled");
				tphw->cap_pressure = false;
			} else {
				tpinfo->min_pressure_lo = r.lower;
				tpinfo->min_pressure_hi = r.upper;
			}
		}
		if (tpinfo->min_pressure_hi > ai.maximum ||
		    tpinfo->min_pressure_hi < ai.minimum ||
		    tpinfo->min_pressure_lo > ai.maximum ||
		    tpinfo->min_pressure_lo < ai.minimum) {
			debug("discarding out-of-bounds pressure range %d:%d",
			    tpinfo->min_pressure_hi, tpinfo->min_pressure_lo);
			tphw->cap_pressure = false;
		}
		quirks_get_uint32(q, QUIRK_ATTR_PALM_PRESSURE_THRESHOLD,
		    &tpinfo->max_pressure);
		quirks_get_uint32(q, MOUSED_TAP_PRESSURE_THRESHOLD,
		    &tpinfo->tap_threshold);
	}
	/* XXX: libinput uses ABS_MT_TOUCH_MAJOR where available */
	if (bit_test(abs_bits, ABS_TOOL_WIDTH) &&
	    quirks_get_uint32(q, QUIRK_ATTR_PALM_SIZE_THRESHOLD,
	     &tpinfo->max_width) && tpinfo->max_width != 0)
		tphw->cap_width = true;
	if (bit_test(abs_bits, ABS_MT_SLOT) &&
	    bit_test(abs_bits, ABS_MT_TRACKING_ID) &&
	    bit_test(abs_bits, ABS_MT_POSITION_X) &&
	    bit_test(abs_bits, ABS_MT_POSITION_Y))
		tphw->is_mt = true;
	if (ioctl(rodent.mfd, EVIOCGPROP(sizeof(prop_bits)), prop_bits) >= 0) {
		if (bit_test(prop_bits, INPUT_PROP_BUTTONPAD))
			tphw->is_clickpad = true;
	}
	/* Set bottom quarter as 42% - 16% - 42% sized softbuttons */
	if (tphw->is_clickpad) {
		i = 25;
		quirks_get_int32(q, MOUSED_SOFTBUTTONS_Y, &i);
		tpinfo->softbuttons_y = sz_y * i / 100;
		if (bit_test(prop_bits, INPUT_PROP_TOPBUTTONPAD))
			tpinfo->softbuttons_y = -tpinfo->softbuttons_y;
		u = 42;
		quirks_get_uint32(q, MOUSED_SOFTBUTTON2_X, &u);
		tpinfo->softbutton2_x = sz_x * u / 100;
		u = 58;
		quirks_get_uint32(q, MOUSED_SOFTBUTTON3_X, &u);
		tpinfo->softbutton3_x = sz_x * u / 100;
	}
	/* Normalize pointer movement to match 200dpi mouse */
	rodent.accel.accelx *= DFLT_MOUSE_RESOLUTION;
	rodent.accel.accelx /= tphw->res_x;
	rodent.accel.accely *= DFLT_MOUSE_RESOLUTION;
	rodent.accel.accely /= tphw->res_y;
	rodent.accel.accelz *= DFLT_MOUSE_RESOLUTION;
	rodent.accel.accelz /= (tphw->res_x * DFLT_LINEHEIGHT);
}

static void
r_init_drift(void)
{
	struct quirks *q = rodent.quirks;
	struct drift *d = &rodent.drift;

	if (opt_drift_terminate) {
		d->terminate = true;
		d->distance = opt_drift_distance;
		d->time = opt_drift_time;
		d->after = opt_drift_after;
	} else if (quirks_get_bool(q, MOUSED_DRIFT_TERMINATE, &d->terminate) &&
		   d->terminate) {
		quirks_get_uint32(q, MOUSED_DRIFT_DISTANCE, &d->distance);
		quirks_get_uint32(q, MOUSED_DRIFT_TIME, &d->time);
		quirks_get_uint32(q, MOUSED_DRIFT_AFTER, &d->after);
	} else
		return;

	if (d->distance == 0 || d->time == 0 || d->after == 0) {
		warnx("invalid drift parameter");
		exit(1);
	}

	debug("terminate drift: distance %d, time %d, after %d",
	    d->distance, d->time, d->after);

	d->time_ts = msec2ts(d->time);
	d->twotime_ts = msec2ts(d->time * 2);
	d->after_ts = msec2ts(d->after);
}

static void
r_init_accel(void)
{
	struct quirks *q = rodent.quirks;
	struct accel *acc = &rodent.accel;
	bool r1, r2;

	acc->accelx = opt_accelx;
	if (opt_accelx == 1.0)
		 quirks_get_double(q, MOUSED_LINEAR_ACCEL_X, &acc->accelx);
	acc->accely = opt_accely;
	if (opt_accely == 1.0)
		 quirks_get_double(q, MOUSED_LINEAR_ACCEL_Y, &acc->accely);
	if (!quirks_get_double(q, MOUSED_LINEAR_ACCEL_Z, &acc->accelz))
		acc->accelz = 1.0;
	acc->lastlength[0] = acc->lastlength[1] = acc->lastlength[2] = 0.0;
	if (opt_exp_accel) {
		acc->is_exponential = true;
		acc->expoaccel = opt_expoaccel;
		acc->expoffset = opt_expoffset;
		return;
	}
	acc->expoaccel = acc->expoffset = 1.0;
	r1 = quirks_get_double(q, MOUSED_EXPONENTIAL_ACCEL, &acc->expoaccel);
	r2 = quirks_get_double(q, MOUSED_EXPONENTIAL_OFFSET, &acc->expoffset);
	if (r1 || r2)
		acc->is_exponential = true;
}

static int
r_init(const char *path)
{
	struct device *dev = &rodent.dev;
	enum device_type type;
	int fd;

	fd = open(path, O_RDWR | O_NONBLOCK);
	if (fd == -1) {
		logwarnx("unable to open %s", path);
		return (-errno);
	}

	switch (type = r_identify(fd)) {
	case DEVICE_TYPE_UNKNOWN:
		debug("cannot determine device type on %s", path);
		close(fd);
		return (-ENOTSUP);
	case DEVICE_TYPE_MOUSE:
	case DEVICE_TYPE_TOUCHPAD:
		break;
	default:
		debug("unsupported device type: %s on %s",
		    r_name(type), path);
		close(fd);
		return (-ENXIO);
	}

	if (grab && ioctl(fd, EVIOCGRAB, 1) == -1) {
		logwarnx("unable to grab %s", path);
		close(fd);
		return (-errno);
	}

	dev->path = path;
	dev->type = type;
	if (ioctl(fd, EVIOCGNAME(sizeof(dev->name) - 1), dev->name) < 0) {
		logwarnx("unable to get device %s name", path);
		close(fd);
		return (-errno);
	}
	/* Do not loop events */
	if (strncmp(dev->name, "System mouse", sizeof(dev->name)) == 0) {
		close(fd);
		return (-ENOTSUP);
	}
	if (ioctl(fd, EVIOCGID, &dev->id) < 0) {
		logwarnx("unable to get device %s ID", path);
		close(fd);
		return (-errno);
	}
	(void)ioctl(fd, EVIOCGUNIQ(sizeof(dev->uniq) - 1), dev->uniq);

	rodent.quirks = quirks_fetch_for_device(quirks, dev);

	rodent.mfd = fd;
	r_init_buttons();
	switch (type) {
	case DEVICE_TYPE_TOUCHPAD:
		r_init_accel();
		r_init_touchpad();
		break;

	case DEVICE_TYPE_MOUSE:
		r_init_accel();
		r_init_drift();
		break;

	default:
//		debug("unsupported evdev type: %s", r_name(rodent.dev.type));
		break;
	}

	quirks_unref(rodent.quirks);

	debug("port: %s  type: %s  model: %s", path, r_name(type), dev->name);

	return (fd);
}

static int
r_protocol(struct input_event *ie, mousestatus_t *act)
{
	struct tpcaps *tphw = &rodent.tphw;
	struct tpinfo *tpinfo = &rodent.tpinfo;

	static int butmapev[8] = {	/* evdev */
	    0,
	    MOUSE_BUTTON1DOWN,
	    MOUSE_BUTTON3DOWN,
	    MOUSE_BUTTON1DOWN | MOUSE_BUTTON3DOWN,
	    MOUSE_BUTTON2DOWN,
	    MOUSE_BUTTON1DOWN | MOUSE_BUTTON2DOWN,
	    MOUSE_BUTTON2DOWN | MOUSE_BUTTON3DOWN,
	    MOUSE_BUTTON1DOWN | MOUSE_BUTTON2DOWN | MOUSE_BUTTON3DOWN
	};
	struct timespec ietime;
	int i, active;

	if (debug > 1)
		debug("received event 0x%02x, 0x%04x, %d",
		    ie->type, ie->code, ie->value);

	switch (ie->type) {
	case EV_REL:
		switch (ie->code) {
		case REL_X:
			ev.dx += ie->value;
			break;
		case REL_Y:
			ev.dy += ie->value;
			break;
		case REL_WHEEL:
			ev.dz += ie->value;
			break;
		case REL_HWHEEL:
			ev.dw += ie->value;
			break;
		}
		break;
	case EV_ABS:
		switch (ie->code) {
		case ABS_X:
			if (!tphw->is_mt)
				ev.dx += ie->value - ev.st.x;
			ev.st.x = ie->value;
			break;
		case ABS_Y:
			if (!tphw->is_mt)
				ev.dy += ie->value - ev.st.y;
			ev.st.y = ie->value;
			break;
		case ABS_PRESSURE:
			ev.st.p = ie->value;
			break;
		case ABS_TOOL_WIDTH:
			ev.st.w = ie->value;
			break;
		case ABS_MT_SLOT:
			if (tphw->is_mt)
				ev.slot = ie->value;
			break;
		case ABS_MT_TRACKING_ID:
			if (tphw->is_mt &&
			    ev.slot >= 0 && ev.slot < MAX_FINGERS) {
				if (ie->value != -1 && ev.mt[ev.slot].id > 0 &&
				    ie->value + 1 != ev.mt[ev.slot].id) {
					debug("tracking id changed %d->%d",
					    ie->value, ev.mt[ev.slot].id - 1);
					ev.mt[ev.slot].id = 0;
				} else
					ev.mt[ev.slot].id = ie->value + 1;
			}
			break;
		case ABS_MT_POSITION_X:
			if (tphw->is_mt &&
			    ev.slot >= 0 && ev.slot < MAX_FINGERS) {
				ev.dx += ie->value - ev.mt[ev.slot].x;
				ev.mt[ev.slot].x = ie->value;
			}
			break;
		case ABS_MT_POSITION_Y:
			if (tphw->is_mt &&
			    ev.slot >= 0 && ev.slot < MAX_FINGERS) {
				ev.dy += ie->value - ev.mt[ev.slot].y;
				ev.mt[ev.slot].y = ie->value;
			}
			break;
		}
		break;
	case EV_KEY:
		switch (ie->code) {
		case BTN_TOUCH:
			ev.st.id = ie->value != 0 ? 1 : 0;
			break;
		case BTN_TOOL_FINGER:
			ev.nfingers = ie->value != 0 ? 1 : ev.nfingers;
			break;
		case BTN_TOOL_DOUBLETAP:
			ev.nfingers = ie->value != 0 ? 2 : ev.nfingers;
			break;
		case BTN_TOOL_TRIPLETAP:
			ev.nfingers = ie->value != 0 ? 3 : ev.nfingers;
			break;
		case BTN_TOOL_QUADTAP:
			ev.nfingers = ie->value != 0 ? 4 : ev.nfingers;
			break;
		case BTN_TOOL_QUINTTAP:
			ev.nfingers = ie->value != 0 ? 5 : ev.nfingers;
			break;
		case BTN_LEFT ... BTN_LEFT + 7:
			ev.buttons &= ~(1 << (ie->code - BTN_LEFT));
			ev.buttons |= ((!!ie->value) << (ie->code - BTN_LEFT));
			break;
		}
		break;
	}

	if ( ie->type != EV_SYN ||
	    (ie->code != SYN_REPORT && ie->code != SYN_DROPPED))
		return (0);

	/*
	 * assembly full package
	 */

	ietime.tv_sec = ie->time.tv_sec;
	ietime.tv_nsec = ie->time.tv_usec * 1000;

	if (!tphw->cap_pressure && ev.st.id != 0)
		ev.st.p = MAX(tpinfo->min_pressure_hi, tpinfo->tap_threshold);
	if (tphw->cap_touch && ev.st.id == 0)
		ev.st.p = 0;

	act->obutton = act->button;
	act->button = butmapev[ev.buttons & MOUSE_SYS_STDBUTTONS];
	act->button |= (ev.buttons & ~MOUSE_SYS_STDBUTTONS);

	/* Convert cumulative to average movement in MT case */
	if (tphw->is_mt) {
		active = 0;
		for (i = 0; i < MAX_FINGERS; i++)
			if (ev.mt[i].id != 0)
				active++;
		/* Do not count finger holding a click as active */
		if (tphw->is_clickpad && ev.buttons != 0)
			active--;
		if (active > 1) {
			/* XXX: We should dynamically update rodent.accel */
			ev.dx /= active;
			ev.dy /= active;
		}
	}

	if (rodent.dev.type == DEVICE_TYPE_TOUCHPAD) {
		if (debug > 1)
			debug("absolute data %d,%d,%d,%d", ev.st.x, ev.st.y,
			    ev.st.p, ev.st.w);
		switch (r_gestures(ev.st.x, ev.st.y, ev.st.p, ev.st.w,
		    ev.nfingers, &ietime, act)) {
		case GEST_IGNORE:
			ev.dx = 0;
			ev.dy = 0;
			ev.dz = 0;
			ev.acc_dx = ev.acc_dy = 0;
			debug("gesture IGNORE");
			break;
		case GEST_ACCUMULATE:	/* Revertable pointer movement. */
			ev.acc_dx += ev.dx;
			ev.acc_dy += ev.dy;
			debug("gesture ACCUMULATE %d,%d", ev.dx, ev.dy);
			ev.dx = 0;
			ev.dy = 0;
			break;
		case GEST_MOVE:		/* Pointer movement. */
			ev.dx += ev.acc_dx;
			ev.dy += ev.acc_dy;
			ev.acc_dx = ev.acc_dy = 0;
			debug("gesture MOVE %d,%d", ev.dx, ev.dy);
			break;
		case GEST_VSCROLL:	/* Vertical scrolling. */
			if (tpinfo->natural_scroll)
				ev.dz = -ev.dy;
			else
				ev.dz = ev.dy;
			ev.dx = -ev.acc_dx;
			ev.dy = -ev.acc_dy;
			ev.acc_dx = ev.acc_dy = 0;
			debug("gesture VSCROLL %d", ev.dz);
			break;
		case GEST_HSCROLL:	/* Horizontal scrolling. */
/*
			if (ev.dx != 0) {
				if (tpinfo->natural_scroll)
					act->button |= (ev.dx > 0)
					    ? MOUSE_BUTTON6DOWN
					    : MOUSE_BUTTON7DOWN;
				else
					act->button |= (ev.dx > 0)
					    ? MOUSE_BUTTON7DOWN
					    : MOUSE_BUTTON6DOWN;
			}
*/
			ev.dx = -ev.acc_dx;
			ev.dy = -ev.acc_dy;
			ev.acc_dx = ev.acc_dy = 0;
			debug("gesture HSCROLL %d", ev.dw);
			break;
		}
	}

	debug("assembled full packet %d,%d,%d", ev.dx, ev.dy, ev.dz);
	act->dx = ev.dx;
	act->dy = ev.dy;
	act->dz = ev.dz;
	ev.dx = ev.dy = ev.dz = ev.dw = 0;

	/*
	 * We don't reset pBufP here yet, as there may be an additional data
	 * byte in some protocols. See above.
	 */

	/* has something changed? */
	act->flags = ((act->dx || act->dy || act->dz) ? MOUSE_POSCHANGED : 0)
	    | (act->obutton ^ act->button);

	return (act->flags);
}

static void
r_vscroll_detect(mousestatus_t *act)
{
	struct scroll *sc = &rodent.scroll;
	mousestatus_t newaction;

	/* Allow middle button drags to scroll up and down */
	if (act->button == MOUSE_BUTTON2DOWN) {
		if (sc->state == SCROLL_NOTSCROLLING) {
			sc->state = SCROLL_PREPARE;
			sc->movement = sc->hmovement = 0;
			debug("PREPARING TO SCROLL");
		}
		return;
	}

	/* This isn't a middle button down... move along... */
	switch (sc->state) {
	case SCROLL_SCROLLING:
		/*
		 * We were scrolling, someone let go of button 2.
		 * Now turn autoscroll off.
		 */
		sc->state = SCROLL_NOTSCROLLING;
		debug("DONE WITH SCROLLING / %d", sc->state);
		break;
	case SCROLL_PREPARE:
		newaction = *act;

		/* We were preparing to scroll, but we never moved... */
		r_timestamp(act);
		r_statetrans(act, &newaction,
			     A(newaction.button & MOUSE_BUTTON1DOWN,
			       act->button & MOUSE_BUTTON3DOWN));

		/* Send middle down */
		newaction.button = MOUSE_BUTTON2DOWN;
		r_click(&newaction);

		/* Send middle up */
		r_timestamp(&newaction);
		newaction.obutton = newaction.button;
		newaction.button = act->button;
		r_click(&newaction);
		break;
	default:
		break;
	}
}

static void
r_vscroll(mousestatus_t *act)
{
	struct scroll *sc = &rodent.scroll;

	switch (sc->state) {
	case SCROLL_PREPARE:
		/* Middle button down, waiting for movement threshold */
		if (act->dy == 0 && act->dx == 0)
			break;
		if (opt_virtual_scroll) {
			sc->movement += act->dy;
			if (sc->movement < -sc->threshold) {
				sc->state = SCROLL_SCROLLING;
			} else if (sc->movement > sc->threshold) {
				sc->state = SCROLL_SCROLLING;
			}
		}
		if (opt_hvirtual_scroll) {
			sc->hmovement += act->dx;
			if (sc->hmovement < -sc->threshold) {
				sc->state = SCROLL_SCROLLING;
			} else if (sc->hmovement > sc->threshold) {
				sc->state = SCROLL_SCROLLING;
			}
		}
		if (sc->state == SCROLL_SCROLLING)
			sc->movement = sc->hmovement = 0;
		break;
	case SCROLL_SCROLLING:
		if (opt_virtual_scroll) {
			sc->movement += act->dy;
			debug("SCROLL: %d", sc->movement);
			if (sc->movement < -sc->speed) {
				/* Scroll down */
				act->dz = -1;
				sc->movement = 0;
			}
			else if (sc->movement > sc->speed) {
				/* Scroll up */
				act->dz = 1;
				sc->movement = 0;
			}
		}
		if (opt_hvirtual_scroll) {
			sc->hmovement += act->dx;
			debug("HORIZONTAL SCROLL: %d", sc->hmovement);

			if (sc->hmovement < -sc->speed) {
				act->dz = -2;
				sc->hmovement = 0;
			}
			else if (sc->hmovement > sc->speed) {
				act->dz = 2;
				sc->hmovement = 0;
			}
		}

		/* Don't move while scrolling */
		act->dx = act->dy = 0;
		break;
	default:
		break;
	}
}

static bool
r_drift (struct drift *drift, mousestatus_t *act)
{
	struct timespec tmp;

	/* X or/and Y movement only - possibly drift */
	tssub(&drift->current_ts, &drift->last_activity, &tmp);
	if (tscmp(&tmp, &drift->after_ts, >)) {
		tssub(&drift->current_ts, &drift->since, &tmp);
		if (tscmp(&tmp, &drift->time_ts, <)) {
			drift->last.x += act->dx;
			drift->last.y += act->dy;
		} else {
			/* discard old accumulated steps (drift) */
			if (tscmp(&tmp, &drift->twotime_ts, >))
				drift->previous.x = drift->previous.y = 0;
			else
				drift->previous = drift->last;
			drift->last.x = act->dx;
			drift->last.y = act->dy;
			drift->since = drift->current_ts;
		}
		if (abs(drift->last.x) + abs(drift->last.y) > drift->distance){
			/* real movement, pass all accumulated steps */
			act->dx = drift->previous.x + drift->last.x;
			act->dy = drift->previous.y + drift->last.y;
			/* and reset accumulators */
			tsclr(&drift->since);
			drift->last.x = drift->last.y = 0;
			/* drift_previous will be cleared at next movement*/
			drift->last_activity = drift->current_ts;
		} else {
			return (true);	/* don't pass current movement to
					 * console driver */
		}
	}
	return (false);
}

static int
r_statetrans(mousestatus_t *a1, mousestatus_t *a2, int trans)
{
	bool changed;
	int flags;

	a2->dx = a1->dx;
	a2->dy = a1->dy;
	a2->dz = a1->dz;
	a2->obutton = a2->button;
	a2->button = a1->button;
	a2->flags = a1->flags;
	changed = false;

	if (rodent.flags & Emulate3Button) {
		if (debug > 2)
			debug("state:%d, trans:%d -> state:%d",
			    mouse_button_state, trans,
			    states[mouse_button_state].s[trans]);
		/*
		 * Avoid re-ordering button and movement events. While a button
		 * event is deferred, throw away up to BUTTON2_MAXMOVE movement
		 * events to allow for mouse jitter. If more movement events
		 * occur, then complete the deferred button events immediately.
		 */
		if ((a2->dx != 0 || a2->dy != 0) &&
		    S_DELAYED(states[mouse_button_state].s[trans])) {
			if (++mouse_move_delayed > BUTTON2_MAXMOVE) {
				mouse_move_delayed = 0;
				mouse_button_state =
				    states[mouse_button_state].s[A_TIMEOUT];
				changed = true;
			} else
				a2->dx = a2->dy = 0;
		} else
			mouse_move_delayed = 0;
		if (mouse_button_state != states[mouse_button_state].s[trans])
			changed = true;
		if (changed)
			clock_gettime(CLOCK_MONOTONIC_FAST,
			   &mouse_button_state_ts);
		mouse_button_state = states[mouse_button_state].s[trans];
		a2->button &= ~(MOUSE_BUTTON1DOWN | MOUSE_BUTTON2DOWN |
		    MOUSE_BUTTON3DOWN);
		a2->button &= states[mouse_button_state].mask;
		a2->button |= states[mouse_button_state].buttons;
		flags = a2->flags & MOUSE_POSCHANGED;
		flags |= a2->obutton ^ a2->button;
		if (flags & MOUSE_BUTTON2DOWN) {
			a2->flags = flags & MOUSE_BUTTON2DOWN;
			r_timestamp(a2);
		}
		a2->flags = flags;
	}
	return (changed);
}

/* phisical to logical button mapping */
static int p2l[MOUSE_MAXBUTTON] = {
    MOUSE_BUTTON1DOWN, MOUSE_BUTTON2DOWN, MOUSE_BUTTON3DOWN, MOUSE_BUTTON4DOWN,
    MOUSE_BUTTON5DOWN, MOUSE_BUTTON6DOWN, MOUSE_BUTTON7DOWN, MOUSE_BUTTON8DOWN,
    0x00000100,        0x00000200,        0x00000400,        0x00000800,
    0x00001000,        0x00002000,        0x00004000,        0x00008000,
    0x00010000,        0x00020000,        0x00040000,        0x00080000,
    0x00100000,        0x00200000,        0x00400000,        0x00800000,
    0x01000000,        0x02000000,        0x04000000,        0x08000000,
    0x10000000,        0x20000000,        0x40000000,
};

static char *
skipspace(char *s)
{
	while(isspace(*s))
		++s;
	return (s);
}

static bool
r_installmap(char *arg)
{
	int pbutton;
	int lbutton;
	char *s;

	while (*arg) {
		arg = skipspace(arg);
		s = arg;
		while (isdigit(*arg))
			++arg;
		arg = skipspace(arg);
		if ((arg <= s) || (*arg != '='))
			return (false);
		lbutton = atoi(s);

		arg = skipspace(++arg);
		s = arg;
		while (isdigit(*arg))
			++arg;
		if ((arg <= s) || (!isspace(*arg) && (*arg != '\0')))
			return (false);
		pbutton = atoi(s);

		if ((lbutton <= 0) || (lbutton > MOUSE_MAXBUTTON))
			return (false);
		if ((pbutton <= 0) || (pbutton > MOUSE_MAXBUTTON))
			return (false);
		p2l[pbutton - 1] = 1 << (lbutton - 1);
		mstate[lbutton - 1] = &bstate[pbutton - 1];
	}

	return (true);
}

static void
r_map(mousestatus_t *act1, mousestatus_t *act2)
{
	int pb;
	int pbuttons;
	int lbuttons;

	pbuttons = act1->button;
	lbuttons = 0;

	act2->obutton = act2->button;
	if (pbuttons & rodent.wmode) {
		pbuttons &= ~rodent.wmode;
		act1->dz = act1->dy;
		act1->dx = 0;
		act1->dy = 0;
	}
	act2->dx = act1->dx;
	act2->dy = act1->dy;
	act2->dz = act1->dz;

	switch (rodent.zmap[0]) {
	case 0:	/* do nothing */
		break;
	case MOUSE_XAXIS:
		if (act1->dz != 0) {
			act2->dx = act1->dz;
			act2->dz = 0;
		}
		break;
	case MOUSE_YAXIS:
		if (act1->dz != 0) {
			act2->dy = act1->dz;
			act2->dz = 0;
		}
		break;
	default:	/* buttons */
		pbuttons &= ~(rodent.zmap[0] | rodent.zmap[1]
			    | rodent.zmap[2] | rodent.zmap[3]);
		if ((act1->dz < -1) && rodent.zmap[2]) {
			pbuttons |= rodent.zmap[2];
			zstate[2].count = 1;
		} else if (act1->dz < 0) {
			pbuttons |= rodent.zmap[0];
			zstate[0].count = 1;
		} else if ((act1->dz > 1) && rodent.zmap[3]) {
			pbuttons |= rodent.zmap[3];
			zstate[3].count = 1;
		} else if (act1->dz > 0) {
			pbuttons |= rodent.zmap[1];
			zstate[1].count = 1;
		}
		act2->dz = 0;
		break;
	}

	for (pb = 0; (pb < MOUSE_MAXBUTTON) && (pbuttons != 0); ++pb) {
		lbuttons |= (pbuttons & 1) ? p2l[pb] : 0;
		pbuttons >>= 1;
	}
	act2->button = lbuttons;

	act2->flags =
	    ((act2->dx || act2->dy || act2->dz) ? MOUSE_POSCHANGED : 0)
	    | (act2->obutton ^ act2->button);
}

static void
r_timestamp(mousestatus_t *act)
{
	struct timespec ts;
	struct timespec ts1;
	struct timespec ts2;
	int button;
	int mask;
	int i;

	mask = act->flags & MOUSE_BUTTONS;
#if 0
	if (mask == 0)
		return;
#endif

	clock_gettime(CLOCK_MONOTONIC_FAST, &ts1);
	rodent.drift.current_ts = ts1;

	/* double click threshold */
	ts = tssubms(&ts1, rodent.clickthreshold);
	debug("ts:  %jd %ld", (intmax_t)ts.tv_sec, ts.tv_nsec);

	/* 3 button emulation timeout */
	ts2 = tssubms(&ts1, rodent.button2timeout);

	button = MOUSE_BUTTON1DOWN;
	for (i = 0; (i < MOUSE_MAXBUTTON) && (mask != 0); ++i) {
		if (mask & 1) {
			if (act->button & button) {
				/* the button is down */
				debug("  :  %jd %ld",
				    (intmax_t)bstate[i].ts.tv_sec,
				    bstate[i].ts.tv_nsec);
				if (tscmp(&ts, &bstate[i].ts, >)) {
					bstate[i].count = 1;
				} else {
					++bstate[i].count;
				}
				bstate[i].ts = ts1;
			} else {
				/* the button is up */
				bstate[i].ts = ts1;
			}
		} else {
			if (act->button & button) {
				/* the button has been down */
				if (tscmp(&ts2, &bstate[i].ts, >)) {
					bstate[i].count = 1;
					bstate[i].ts = ts1;
					act->flags |= button;
					debug("button %d timeout", i + 1);
				}
			} else {
				/* the button has been up */
			}
		}
		button <<= 1;
		mask >>= 1;
	}
}

static bool
r_timeout(void)
{
	struct timespec ts;
	struct timespec ts1;

	if (states[mouse_button_state].timeout)
		return (true);
	clock_gettime(CLOCK_MONOTONIC_FAST, &ts1);
	ts = tssubms(&ts1, rodent.button2timeout);
	return (tscmp(&ts, &mouse_button_state_ts, >));
}

static void
r_click(mousestatus_t *act)
{
	struct mouse_info mouse;
	int button;
	int mask;
	int i;

	mask = act->flags & MOUSE_BUTTONS;
	if (mask == 0)
		return;

	button = MOUSE_BUTTON1DOWN;
	for (i = 0; (i < MOUSE_MAXBUTTON) && (mask != 0); ++i) {
		if (mask & 1) {
			debug("mstate[%d]->count:%d", i, mstate[i]->count);
			if (act->button & button) {
				/* the button is down */
				mouse.u.event.value = mstate[i]->count;
			} else {
				/* the button is up */
				mouse.u.event.value = 0;
			}
			mouse.operation = MOUSE_BUTTON_EVENT;
			mouse.u.event.id = button;
			if (debug < 2 && !paused)
				ioctl(rodent.cfd, CONS_MOUSECTL, &mouse);
			debug("button %d  count %d", i + 1,
			    mouse.u.event.value);
		}
		button <<= 1;
		mask >>= 1;
	}
}

static enum gesture
r_gestures(int x0, int y0, int z, int w, int nfingers, struct timespec *time,
    mousestatus_t *ms)
{
	struct gesture_state *gest = &gesture;
	struct tpcaps *tphw = &rodent.tphw;
	struct tpinfo *tpinfo = &rodent.tpinfo;
	int tap_timeout = tpinfo->tap_timeout;

	/*
	 * Check pressure to detect a real wanted action on the
	 * touchpad.
	 */
	if (z >= tpinfo->min_pressure_hi ||
	    (gest->fingerdown && z >= tpinfo->min_pressure_lo)) {
		/* XXX Verify values? */
		bool two_finger_scroll = tpinfo->two_finger_scroll;
		bool three_finger_drag = tpinfo->three_finger_drag;
		int max_width = tpinfo->max_width;
		int max_pressure = tpinfo->max_pressure;
		int margin_top = tpinfo->margin_top;
		int margin_right = tpinfo->margin_right;
		int margin_bottom = tpinfo->margin_bottom;
		int margin_left = tpinfo->margin_left;
		int vscroll_hor_area = tpinfo->vscroll_hor_area * tphw->res_x;
		int vscroll_ver_area = tpinfo->vscroll_ver_area * tphw->res_y;;

		int max_x = tphw->max_x;
		int max_y = tphw->max_y;
		int min_x = tphw->min_x;
		int min_y = tphw->min_y;

		int dx, dy;
		int start_x, start_y;
		int tap_max_delta_x, tap_max_delta_y;
		int prev_nfingers;

		/* Palm detection. */
		if (nfingers == 1 &&
		    ((tphw->cap_width && w > max_width) ||
		     (tphw->cap_pressure && z > max_pressure))) {
			/*
			 * We consider the packet irrelevant for the current
			 * action when:
			 *  - there is a single active touch
			 *  - the width isn't comprised in:
			 *    [0; max_width]
			 *  - the pressure isn't comprised in:
			 *    [min_pressure; max_pressure]
			 *
			 *  Note that this doesn't terminate the current action.
			 */
			debug("palm detected! (%d)", z);
			return(GEST_IGNORE);
		}

		/*
		 * Limit the coordinates to the specified margins because
		 * this area isn't very reliable.
		 */
		if (margin_left != 0 && x0 <= min_x + margin_left)
			x0 = min_x + margin_left;
		else if (margin_right != 0 && x0 >= max_x - margin_right)
			x0 = max_x - margin_right;
		if (margin_bottom != 0 && y0 <= min_y + margin_bottom)
			y0 = min_y + margin_bottom;
		else if (margin_top != 0 && y0 >= max_y - margin_top)
			y0 = max_y - margin_top;

		debug("packet: [%d, %d], %d, %d", x0, y0, z, w);

		/*
		 * If the action is just beginning, init the structure and
		 * compute tap timeout.
		 */
		if (!gest->fingerdown) {
			debug("----");

			/* Reset pressure peak. */
			gest->zmax = 0;

			/* Reset fingers count. */
			gest->fingers_nb = 0;

			/* Reset virtual scrolling state. */
			gest->in_vscroll = 0;

			/* Compute tap timeout. */
			if (tap_timeout != 0)
				gest->taptimeout = tsaddms(time, tap_timeout);
			else
				tsclr(&gest->taptimeout);

			gest->startdelay = tsaddms(time, 25);

			gest->fingerdown = true;

			gest->start_x = x0;
			gest->start_y = y0;
		}

		prev_nfingers = gest->prev_nfingers;

		gest->prev_x = x0;
		gest->prev_y = y0;
		gest->prev_nfingers = nfingers;

		start_x = gest->start_x;
		start_y = gest->start_y;

		/* Process ClickPad softbuttons */
		if (tphw->is_clickpad && ms->button & MOUSE_BUTTON1DOWN) {
			int y_ok, center_bt, center_x, right_bt, right_x;
			y_ok = tpinfo->softbuttons_y < 0
			    ? start_y < min_y - tpinfo->softbuttons_y
			    : start_y > max_y - tpinfo->softbuttons_y;

			center_bt = MOUSE_BUTTON2DOWN;
			center_x = min_x + tpinfo->softbutton2_x;
			right_bt = MOUSE_BUTTON3DOWN;
			right_x = min_x + tpinfo->softbutton3_x;

			if (center_x > 0 && right_x > 0 && center_x > right_x) {
				center_bt = MOUSE_BUTTON3DOWN;
				center_x = min_x + tpinfo->softbutton3_x;
				right_bt = MOUSE_BUTTON2DOWN;
				right_x = min_x + tpinfo->softbutton2_x;
			}

			if (right_x > 0 && start_x > right_x && y_ok)
				ms->button = (ms->button &
				    ~MOUSE_BUTTON1DOWN) | right_bt;
			else if (center_x > 0 && start_x > center_x && y_ok)
				ms->button = (ms->button &
				    ~MOUSE_BUTTON1DOWN) | center_bt;
		}

		/* If in tap-hold or three fingers, add the recorded button. */
		if (gest->in_taphold || (nfingers == 3 && three_finger_drag))
			ms->button |= gest->tap_button;

		/*
		 * For tap, we keep the maximum number of fingers and the
		 * pressure peak.
		 */
		gest->fingers_nb = MAX(nfingers, gest->fingers_nb);
		gest->zmax = MAX(z, gest->zmax);

		/* Ignore few events at beginning. They are often noisy */
		if (tscmp(time, &gest->startdelay, <=)) {
			gest->start_x = x0;
			gest->start_y = y0;
			return (GEST_IGNORE);
		}

		dx = abs(x0 - start_x);
		dy = abs(y0 - start_y);

		/*
		 * A scrolling action must not conflict with a tap action.
		 * Here are the conditions to consider a scrolling action:
		 *  - the action in a configurable area
		 *  - one of the following:
		 *     . the distance between the last packet and the
		 *       first should be above a configurable minimum
		 *     . tap timed out
		 */
		if (!gest->in_taphold && !ms->button &&
		    (!gest->in_vscroll || two_finger_scroll) &&
		    (tscmp(time, &gest->taptimeout, >) ||
		     dx >= tpinfo->vscroll_min_delta * tphw->res_x ||
		     dy >= tpinfo->vscroll_min_delta * tphw->res_y)) {
			/*
			 * Handle two finger scrolling.
			 * Note that we don't rely on fingers_nb
			 * as that keeps the maximum number of fingers.
			 */
			if (two_finger_scroll) {
				if (nfingers == 2) {
					gest->in_vscroll += dy ? 2 : 0;
					gest->in_vscroll += dx ? 1 : 0;
				}
			} else {
				/* Check for horizontal scrolling. */
				if ((vscroll_hor_area > 0 &&
				     start_y <= min_y + vscroll_hor_area) ||
				    (vscroll_hor_area < 0 &&
				     start_y >= max_y + vscroll_hor_area))
					gest->in_vscroll += 2;

				/* Check for vertical scrolling. */
				if ((vscroll_ver_area > 0 &&
				     start_x <= min_x + vscroll_ver_area) ||
				    (vscroll_ver_area < 0 &&
				     start_x >= max_x + vscroll_ver_area))
					gest->in_vscroll += 1;
			}
			/* Avoid conflicts if area overlaps. */
			if (gest->in_vscroll >= 3)
				gest->in_vscroll = (dx > dy) ? 2 : 1;
		}
		/*
		 * Reset two finger scrolling when the number of fingers
		 * is different from two or any button is pressed.
		 */
		if (two_finger_scroll && gest->in_vscroll != 0 &&
		    (nfingers != 2 || ms->button))
			gest->in_vscroll = 0;

		debug("virtual scrolling: %s "
			"(direction=%d, dx=%d, dy=%d, fingers=%d)",
			gest->in_vscroll != 0 ? "YES" : "NO",
			gest->in_vscroll, dx, dy, gest->fingers_nb);

		/* Workaround cursor jump on finger set changes */
		if (prev_nfingers != nfingers)
			return (GEST_IGNORE);

		switch (gest->in_vscroll) {
		case 1:
			return (GEST_VSCROLL);
		case 2:
			return (GEST_HSCROLL);
		default:
			/* NO-OP */;
		}

		/* Max delta is disabled for multi-fingers tap. */
		if (gest->fingers_nb == 1 &&
		    tscmp(time, &gest->taptimeout, <=)) {
			tap_max_delta_x = tpinfo->tap_max_delta * tphw->res_x;
			tap_max_delta_y = tpinfo->tap_max_delta * tphw->res_y;

			debug("dx=%d, dy=%d, deltax=%d, deltay=%d",
			    dx, dy, tap_max_delta_x, tap_max_delta_y);
			if (dx > tap_max_delta_x || dy > tap_max_delta_y) {
				debug("not a tap");
				tsclr(&gest->taptimeout);
			}
		}

		if (tscmp(time, &gest->taptimeout, <=))
			return (gest->fingers_nb > 1 ?
			    GEST_IGNORE : GEST_ACCUMULATE);
		else
			return (GEST_MOVE);
	}

	/*
	 * Handle a case when clickpad pressure drops before than
	 * button up event when surface is released after click.
	 * It interferes with softbuttons.
	 */
	if (tphw->is_clickpad && tpinfo->softbuttons_y != 0)
		ms->button &= ~MOUSE_BUTTON1DOWN;

	gest->prev_nfingers = 0;

	if (gest->fingerdown) {
		/*
		 * An action is currently taking place but the pressure
		 * dropped under the minimum, putting an end to it.
		 */

		gest->fingerdown = false;

		/* Check for tap. */
		debug("zmax=%d fingers=%d", gest->zmax, gest->fingers_nb);
		if (!gest->in_vscroll && gest->zmax >= tpinfo->tap_threshold &&
		    tscmp(time, &gest->taptimeout, <=)) {
			/*
			 * We have a tap if:
			 *   - the maximum pressure went over tap_threshold
			 *   - the action ended before tap_timeout
			 *
			 * To handle tap-hold, we must delay any button push to
			 * the next action.
			 */
			if (gest->in_taphold) {
				/*
				 * This is the second and last tap of a
				 * double tap action, not a tap-hold.
				 */
				gest->in_taphold = false;

				/*
				 * For double-tap to work:
				 *   - no button press is emitted (to
				 *     simulate a button release)
				 *   - PSM_FLAGS_FINGERDOWN is set to
				 *     force the next packet to emit a
				 *     button press)
				 */
				debug("button RELEASE: %d", gest->tap_button);
				gest->fingerdown = true;

				/* Schedule button press on next event */
				gest->idletimeout = 0;
			} else {
				/*
				 * This is the first tap: we set the
				 * tap-hold state and notify the button
				 * down event.
				 */
				gest->in_taphold = true;
				gest->idletimeout = tpinfo->taphold_timeout;
				gest->taptimeout = tsaddms(time, tap_timeout);

				switch (gest->fingers_nb) {
				case 3:
					gest->tap_button =
					    MOUSE_BUTTON2DOWN;
					break;
				case 2:
					gest->tap_button =
					    MOUSE_BUTTON3DOWN;
					break;
				default:
					gest->tap_button =
					    MOUSE_BUTTON1DOWN;
				}
				debug("button PRESS: %d", gest->tap_button);
				ms->button |= gest->tap_button;
			}
		} else {
			/*
			 * Not enough pressure or timeout: reset
			 * tap-hold state.
			 */
			if (gest->in_taphold) {
				debug("button RELEASE: %d", gest->tap_button);
				gest->in_taphold = false;
			} else {
				debug("not a tap-hold");
			}
		}
	} else if (!gest->fingerdown && gest->in_taphold) {
		/*
		 * For a tap-hold to work, the button must remain down at
		 * least until timeout (where the in_taphold flags will be
		 * cleared) or during the next action.
		 */
		if (tscmp(time, &gest->taptimeout, <=)) {
			ms->button |= gest->tap_button;
		} else {
			debug("button RELEASE: %d", gest->tap_button);
			gest->in_taphold = false;
		}
	}

	return (GEST_IGNORE);
}
