/**
 ** SPDX-License-Identifier: BSD-4-Clause
 **
 ** Copyright (c) 1995 Michael Smith, All rights reserved.
 **
 ** Redistribution and use in source and binary forms, with or without
 ** modification, are permitted provided that the following conditions
 ** are met:
 ** 1. Redistributions of source code must retain the above copyright
 **    notice, this list of conditions and the following disclaimer as
 **    the first lines of this file unmodified.
 ** 2. Redistributions in binary form must reproduce the above copyright
 **    notice, this list of conditions and the following disclaimer in the
 **    documentation and/or other materials provided with the distribution.
 ** 3. All advertising materials mentioning features or use of this software
 **    must display the following acknowledgment:
 **      This product includes software developed by Michael Smith.
 ** 4. The name of the author may not be used to endorse or promote products
 **    derived from this software without specific prior written permission.
 **
 **
 ** THIS SOFTWARE IS PROVIDED BY Michael Smith ``AS IS'' AND ANY
 ** EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 ** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 ** PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL Michael Smith BE LIABLE FOR
 ** ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 ** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 ** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 ** BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 ** WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 ** OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 ** EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **
 **/

/**
 ** MOUSED.C
 **
 ** Mouse daemon : listens to a serial port, the bus mouse interface, or
 ** the PS/2 mouse port for mouse data stream, interprets data and passes
 ** ioctls off to the console driver.
 **
 ** The mouse interface functions are derived closely from the mouse
 ** handler in the XFree86 X server.  Many thanks to the XFree86 people
 ** for their great work!
 **
 **/

#include <sys/cdefs.h>
__FBSDID("$FreeBSD$");

#include <sys/param.h>
#include <sys/consio.h>
#include <sys/mouse.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <limits.h>
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
#include <termios.h>
#include <unistd.h>
#include <math.h>

#define MAX_CLICKTHRESHOLD	2000	/* 2 seconds */
#define MAX_BUTTON2TIMEOUT	2000	/* 2 seconds */
#define DFLT_CLICKTHRESHOLD	 500	/* 0.5 second */
#define DFLT_BUTTON2TIMEOUT	 100	/* 0.1 second */
#define DFLT_SCROLLTHRESHOLD	   3	/* 3 pixels */
#define DFLT_SCROLLSPEED	   2	/* 2 pixels */

/* Abort 3-button emulation delay after this many movement events. */
#define BUTTON2_MAXMOVE	3

#define TRUE		1
#define FALSE		0

#define MOUSE_XAXIS	(-1)
#define MOUSE_YAXIS	(-2)

#define	ChordMiddle	0x0001
#define Emulate3Button	0x0002
#define VirtualScroll	0x0020
#define HVirtualScroll	0x0040
#define ExponentialAcc	0x0080

#define ID_NONE		0
#define ID_PORT		1
/* Was	ID_IF		2 */
#define ID_TYPE		4
#define ID_MODEL	8
#define ID_ALL		(ID_PORT | ID_TYPE | ID_MODEL)

/* Operations on timespecs */
#define	tsclr(tvp)	((tvp)->tv_sec = (tvp)->tv_nsec = 0)
#define	tscmp(tvp, uvp, cmp)						\
	(((tvp)->tv_sec == (uvp)->tv_sec) ?				\
	    ((tvp)->tv_nsec cmp (uvp)->tv_nsec) :			\
	    ((tvp)->tv_sec cmp (uvp)->tv_sec))
#define	tssub(tvp, uvp, vvp)						\
	do {								\
		(vvp)->tv_sec = (tvp)->tv_sec - (uvp)->tv_sec;		\
		(vvp)->tv_nsec = (tvp)->tv_nsec - (uvp)->tv_nsec;	\
		if ((vvp)->tv_nsec < 0) {				\
			(vvp)->tv_sec--;				\
			(vvp)->tv_nsec += 1000000000;			\
		}							\
	} while (0)

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

/* structures */

/* symbol table entry */
typedef struct {
    const char *name;
    int val;
    int val2;
} symtab_t;

/* global variables */

static int	debug = 0;
static int	nodaemon = FALSE;
static int	background = FALSE;
static int	paused = FALSE;
static int	identify = ID_NONE;
static int	extioctl = FALSE;
static const char *pidfile = "/var/run/moused.pid";
static struct pidfh *pfh;

#define SCROLL_NOTSCROLLING	0
#define SCROLL_PREPARE		1
#define SCROLL_SCROLLING	2

static int	scroll_state;
static int	scroll_movement;
static int	hscroll_movement;

/* local variables */

#include <sys/bitstring.h>
#include <dev/evdev/input.h>
/*
 * bitstr_t implementation must be identical to one found in EVIOCG*
 * libevdev ioctls. Our bitstring(3) API is compatible since r299090.
 */
_Static_assert(sizeof(bitstr_t) == sizeof(unsigned long),
    "bitstr_t size mismatch");

#define MOUSE_IF_EVDEV		6
#define MOUSE_PROTO_MOUSE	0
#define MOUSE_PROTO_TOUCHPAD	1
#define MOUSE_PROTO_TOUCHSCREEN	2
#define MOUSE_PROTO_TABLET	3
#define MOUSE_PROTO_KEYBOARD	4
#define MOUSE_PROTO_JOYSTICK	5

/* interface (the table must be ordered by MOUSE_IF_XXX in mouse.h) */
static symtab_t rifs[] = {
    { "evdev",		MOUSE_IF_EVDEV,		0 },
    { NULL,		MOUSE_IF_UNKNOWN,	0 },
};

/* types (the table must be ordered by MOUSE_PROTO_XXX in mouse.h) */
static const char *rnames[] = {
    "mouse",
    "touchpad",
    "touchscreen",
    "tablet",
    "keyboard",
    "joystick",
    NULL
};

static struct rodentparam {
    int flags;
    const char *portname;	/* /dev/XXX */
    int rtype;			/* MOUSE_PROTO_XXX */
    int zmap[4];		/* MOUSE_{X|Y}AXIS or a button number */
    int wmode;			/* wheel mode button number */
    int mfd;			/* mouse file descriptor */
    int cfd;			/* /dev/consolectl file descriptor */
    long clickthreshold;	/* double click speed in msec */
    long button2timeout;	/* 3 button emulation timeout */
    char model[80];		/* mouse name */
    float accelx;		/* Acceleration in the X axis */
    float accely;		/* Acceleration in the Y axis */
    float expoaccel;		/* Exponential acceleration */
    float expoffset;		/* Movement offset for exponential accel. */
    float remainx;		/* Remainder on X and Y axis, respectively... */
    float remainy;		/*    ... to compensate for rounding errors. */
    int scrollthreshold;	/* Movement distance before virtual scrolling */
    int scrollspeed;		/* Movement distance to rate of scrolling */
    bitstr_t bit_decl(key_bits, KEY_CNT); /* */
    bitstr_t bit_decl(rel_bits, REL_CNT); /* Evdev capabilities */
    bitstr_t bit_decl(abs_bits, ABS_CNT); /* */
} rodent = {
    .flags = 0,
    .portname = NULL,
    .rtype = MOUSE_PROTO_UNKNOWN,
    .zmap = { 0, 0, 0, 0 },
    .wmode = 0,
    .mfd = -1,
    .cfd = -1,
    .clickthreshold = DFLT_CLICKTHRESHOLD,
    .button2timeout = DFLT_BUTTON2TIMEOUT,
    .accelx = 1.0,
    .accely = 1.0,
    .expoaccel = 1.0,
    .expoffset = 1.0,
    .remainx = 0.0,
    .remainy = 0.0,
    .scrollthreshold = DFLT_SCROLLTHRESHOLD,
    .scrollspeed = DFLT_SCROLLSPEED,
};

/* button status */
struct button_state {
    int count;		/* 0: up, 1: single click, 2: double click,... */
    struct timespec ts;	/* timestamp on the last button event */
};
static struct button_state	bstate[MOUSE_MAXBUTTON];	/* button state */
static struct button_state	*mstate[MOUSE_MAXBUTTON];/* mapped button st.*/
static struct button_state	zstate[4];		 /* Z/W axis state */

/* state machine for 3 button emulation */

#define S0	0	/* start */
#define S1	1	/* button 1 delayed down */
#define S2	2	/* button 3 delayed down */
#define S3	3	/* both buttons down -> button 2 down */
#define S4	4	/* button 1 delayed up */
#define S5	5	/* button 1 down */
#define S6	6	/* button 3 down */
#define S7	7	/* both buttons down */
#define S8	8	/* button 3 delayed up */
#define S9	9	/* button 1 or 3 up after S3 */

#define A(b1, b3)	(((b1) ? 2 : 0) | ((b3) ? 1 : 0))
#define A_TIMEOUT	4
#define S_DELAYED(st)	(states[st].s[A_TIMEOUT] != (st))

static struct {
    int s[A_TIMEOUT + 1];
    int buttons;
    int mask;
    int timeout;
} states[10] = {
    /* S0 */
    { { S0, S2, S1, S3, S0 }, 0, ~(MOUSE_BUTTON1DOWN | MOUSE_BUTTON3DOWN), FALSE },
    /* S1 */
    { { S4, S2, S1, S3, S5 }, 0, ~MOUSE_BUTTON1DOWN, FALSE },
    /* S2 */
    { { S8, S2, S1, S3, S6 }, 0, ~MOUSE_BUTTON3DOWN, FALSE },
    /* S3 */
    { { S0, S9, S9, S3, S3 }, MOUSE_BUTTON2DOWN, ~0, FALSE },
    /* S4 */
    { { S0, S2, S1, S3, S0 }, MOUSE_BUTTON1DOWN, ~0, TRUE },
    /* S5 */
    { { S0, S2, S5, S7, S5 }, MOUSE_BUTTON1DOWN, ~0, FALSE },
    /* S6 */
    { { S0, S6, S1, S7, S6 }, MOUSE_BUTTON3DOWN, ~0, FALSE },
    /* S7 */
    { { S0, S6, S5, S7, S7 }, MOUSE_BUTTON1DOWN | MOUSE_BUTTON3DOWN, ~0, FALSE },
    /* S8 */
    { { S0, S2, S1, S3, S0 }, MOUSE_BUTTON3DOWN, ~0, TRUE },
    /* S9 */
    { { S0, S9, S9, S3, S9 }, 0, ~(MOUSE_BUTTON1DOWN | MOUSE_BUTTON3DOWN), FALSE },
};
static int		mouse_button_state;
static struct timespec	mouse_button_state_ts;
static int		mouse_move_delayed;

static jmp_buf env;

struct drift_xy {
    int x;
    int y;
};
static int		drift_distance = 4;	/* max steps X+Y */
static int		drift_time = 500;	/* in 0.5 sec */
static struct timespec	drift_time_ts;
static struct timespec	drift_2time_ts;		/* 2*drift_time */
static int		drift_after = 4000;	/* 4 sec */
static struct timespec	drift_after_ts;
static int		drift_terminate = FALSE;
static struct timespec	drift_current_ts;
static struct timespec	drift_tmp;
static struct timespec	drift_last_activity = {0, 0};
static struct timespec	drift_since = {0, 0};
static struct drift_xy	drift_last = {0, 0};	/* steps in last drift_time */
static struct drift_xy  drift_previous = {0, 0};	/* steps in prev. drift_time */

/* function prototypes */

static void	linacc(int, int, int*, int*);
static void	expoacc(int, int, int*, int*);
static void	moused(void);
static void	hup(int sig);
static void	cleanup(int sig);
static void	pause_mouse(int sig);
static void	usage(void);
static void	log_or_warn(int log_pri, int errnum, const char *fmt, ...)
		    __printflike(3, 4);

static int	r_identify(void);
static const char *r_name(int type);
static void	r_init(void);
static int	r_protocol(int rtype, struct input_event *b, mousestatus_t *act);
static int	r_statetrans(mousestatus_t *a1, mousestatus_t *a2, int trans);
static int	r_installmap(char *arg);
static void	r_map(mousestatus_t *act1, mousestatus_t *act2);
static void	r_timestamp(mousestatus_t *act);
static int	r_timeout(void);
static void	r_click(mousestatus_t *act);

int
main(int argc, char *argv[])
{
    int c;
    int	i;
    int	j;

    for (i = 0; i < MOUSE_MAXBUTTON; ++i)
	mstate[i] = &bstate[i];

    while ((c = getopt(argc, argv, "3A:C:E:HI:L:T:VU:a:cdfhi:m:p:w:z:")) != -1)
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
	    i = sscanf(optarg, "%f,%f", &rodent.accelx, &rodent.accely);
	    if (i == 0) {
		warnx("invalid linear acceleration argument '%s'", optarg);
		usage();
	    }

	    if (i == 1)
		rodent.accely = rodent.accelx;

	    break;

	case 'A':
	    rodent.flags |= ExponentialAcc;
	    i = sscanf(optarg, "%f,%f", &rodent.expoaccel, &rodent.expoffset);
	    if (i == 0) {
		warnx("invalid exponential acceleration argument '%s'", optarg);
		usage();
	    }

	    if (i == 1)
		rodent.expoffset = 1.0;

	    break;

	case 'c':
	    rodent.flags |= ChordMiddle;
	    break;

	case 'd':
	    ++debug;
	    break;

	case 'f':
	    nodaemon = TRUE;
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
	    nodaemon = TRUE;
	    break;

	case 'm':
	    if (!r_installmap(optarg)) {
		warnx("invalid argument `%s'", optarg);
		usage();
	    }
	    break;

	case 'p':
	    rodent.portname = optarg;
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
	    if (strcmp(optarg, "x") == 0)
		rodent.zmap[0] = MOUSE_XAXIS;
	    else if (strcmp(optarg, "y") == 0)
		rodent.zmap[0] = MOUSE_YAXIS;
	    else {
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
			warnx("invalid argument `%s'", argv[optind]);
			usage();
		    }
		    rodent.zmap[j] = i;
		    ++optind;
		}
		if ((rodent.zmap[2] != 0) && (rodent.zmap[3] == 0))
		    rodent.zmap[3] = rodent.zmap[2] + 1;
	    }
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
	    rodent.flags |= HVirtualScroll;
	    break;
		
	case 'I':
	    pidfile = optarg;
	    break;

	case 'L':
	    rodent.scrollspeed = atoi(optarg);
	    if (rodent.scrollspeed < 0) {
		warnx("invalid argument `%s'", optarg);
		usage();
	    }
	    break;

	case 'T':
	    drift_terminate = TRUE;
	    sscanf(optarg, "%d,%d,%d", &drift_distance, &drift_time,
		&drift_after);
	    if (drift_distance <= 0 || drift_time <= 0 || drift_after <= 0) {
		warnx("invalid argument `%s'", optarg);
		usage();
	    }
	    debug("terminate drift: distance %d, time %d, after %d",
		drift_distance, drift_time, drift_after);
	    drift_time_ts.tv_sec = drift_time / 1000;
	    drift_time_ts.tv_nsec = (drift_time % 1000) * 1000000;
 	    drift_2time_ts.tv_sec = (drift_time *= 2) / 1000;
	    drift_2time_ts.tv_nsec = (drift_time % 1000) * 1000000;
	    drift_after_ts.tv_sec = drift_after / 1000;
	    drift_after_ts.tv_nsec = (drift_after % 1000) * 1000000;
	    break;

	case 'V':
	    rodent.flags |= VirtualScroll;
	    break;
	case 'U':
	    rodent.scrollthreshold = atoi(optarg);
	    if (rodent.scrollthreshold < 0) {
		warnx("invalid argument `%s'", optarg);
		usage();
	    }
	    break;

	case 'h':
	case '?':
	default:
	    usage();
	}

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

    /* the default port name */
    switch(rodent.rtype) {

    default:
	if (rodent.portname)
	    break;
	warnx("no port name specified");
	usage();
    }

	if (setjmp(env) == 0) {
	    signal(SIGHUP, hup);
	    signal(SIGINT , cleanup);
	    signal(SIGQUIT, cleanup);
	    signal(SIGTERM, cleanup);
	    signal(SIGUSR1, pause_mouse);

	    rodent.mfd = open(rodent.portname, O_RDWR | O_NONBLOCK);
	    if (rodent.mfd == -1)
		logerr(1, "unable to open %s", rodent.portname);
	    if (r_identify() == MOUSE_PROTO_UNKNOWN) {
		logwarnx("cannot determine mouse type on %s", rodent.portname);
		close(rodent.mfd);
		rodent.mfd = -1;
	    }

	    /* print some information */
	    if (identify != ID_NONE) {
		if (identify == ID_ALL)
		    printf("%s %s %s\n",
			rodent.portname, r_name(rodent.rtype), rodent.model);
		else if (identify & ID_PORT)
		    printf("%s\n", rodent.portname);
		else if (identify & ID_TYPE)
		    printf("%s\n", r_name(rodent.rtype));
		else if (identify & ID_MODEL)
		    printf("%s\n", rodent.model);
		exit(0);
	    } else {
		debug("port: %s  type: %s  model: %s",
		    rodent.portname, r_name(rodent.rtype), rodent.model);
	    }

	    if (rodent.mfd == -1) {
		/*
		 * We cannot continue because of error.  Exit if the
		 * program has not become a daemon.  Otherwise, block
		 * until the user corrects the problem and issues SIGHUP.
		 */
		if (!background)
		    exit(1);
		sigpause(0);
	    }

	    r_init();			/* call init function */
	    moused();
	}

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
linacc(int dx, int dy, int *movex, int *movey)
{
    float fdx, fdy;

    if (dx == 0 && dy == 0) {
	*movex = *movey = 0;
	return;
    }
    fdx = dx * rodent.accelx + rodent.remainx;
    fdy = dy * rodent.accely + rodent.remainy;
    *movex = lround(fdx);
    *movey = lround(fdy);
    rodent.remainx = fdx - *movex;
    rodent.remainy = fdy - *movey;
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
expoacc(int dx, int dy, int *movex, int *movey)
{
    static float lastlength[3] = {0.0, 0.0, 0.0};
    float fdx, fdy, length, lbase, accel;

    if (dx == 0 && dy == 0) {
	*movex = *movey = 0;
	return;
    }
    fdx = dx * rodent.accelx;
    fdy = dy * rodent.accely;
    length = sqrtf((fdx * fdx) + (fdy * fdy));		/* Pythagoras */
    length = (length + lastlength[0] + lastlength[1] + lastlength[2]) / 4;
    lbase = length / rodent.expoffset;
    accel = powf(lbase, rodent.expoaccel) / lbase;
    fdx = fdx * accel + rodent.remainx;
    fdy = fdy * accel + rodent.remainy;
    *movex = lroundf(fdx);
    *movey = lroundf(fdy);
    rodent.remainx = fdx - *movex;
    rodent.remainy = fdy - *movey;
    lastlength[2] = lastlength[1];
    lastlength[1] = lastlength[0];
    lastlength[0] = length;	/* Insert new average, not original length! */
}

static void
moused(void)
{
    struct mouse_info mouse;
    mousestatus_t action0;		/* original mouse action */
    mousestatus_t action;		/* interim buffer */
    mousestatus_t action2;		/* mapped action */
    int timeout;
    struct pollfd fds;
    struct input_event b;
    pid_t mpid;
    int flags;
    int c;
    int i;

    if ((rodent.cfd = open("/dev/consolectl", O_RDWR, 0)) == -1)
	logerr(1, "cannot open /dev/consolectl");

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
	    background = TRUE;
	    pidfile_write(pfh);
	}
    }

    /* clear mouse data */
    bzero(&action0, sizeof(action0));
    bzero(&action, sizeof(action));
    bzero(&action2, sizeof(action2));
    bzero(&mouse, sizeof(mouse));
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

    /* choose which ioctl command to use */
    mouse.operation = MOUSE_MOTION_EVENT;
    extioctl = (ioctl(rodent.cfd, CONS_MOUSECTL, &mouse) == 0);

    /* process mouse data */
    for (;;) {

	fds.fd = rodent.mfd;
	fds.events = POLLIN;
	fds.revents = 0;
	timeout = ((rodent.flags & Emulate3Button) &&
	    S_DELAYED(mouse_button_state)) ? 20 : -1;

	c = poll(&fds, 1, timeout);
	if (c < 0) {                    /* error */
	    logwarn("failed to read from mouse");
	    continue;
	} else if (c == 0) {            /* timeout */
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
	    if ((fds.revents & POLLIN) == 0)
		return;
	    if (read(rodent.mfd, &b, sizeof(b)) == -1) {
		if (errno == EWOULDBLOCK)
		    continue;
		else
		    return;
	    }
	    if ((flags = r_protocol(rodent.rtype, &b, &action0)) == 0)
		continue;

	    if ((rodent.flags & VirtualScroll) || (rodent.flags & HVirtualScroll)) {
		/* Allow middle button drags to scroll up and down */
		if (action0.button == MOUSE_BUTTON2DOWN) {
		    if (scroll_state == SCROLL_NOTSCROLLING) {
			scroll_state = SCROLL_PREPARE;
			scroll_movement = hscroll_movement = 0;
			debug("PREPARING TO SCROLL");
		    }
		    debug("[BUTTON2] flags:%08x buttons:%08x obuttons:%08x",
			  action.flags, action.button, action.obutton);
		} else {
		    debug("[NOTBUTTON2] flags:%08x buttons:%08x obuttons:%08x",
			  action.flags, action.button, action.obutton);

		    /* This isn't a middle button down... move along... */
		    if (scroll_state == SCROLL_SCROLLING) {
			/*
			 * We were scrolling, someone let go of button 2.
			 * Now turn autoscroll off.
			 */
			scroll_state = SCROLL_NOTSCROLLING;
			debug("DONE WITH SCROLLING / %d", scroll_state);
		    } else if (scroll_state == SCROLL_PREPARE) {
			mousestatus_t newaction = action0;

			/* We were preparing to scroll, but we never moved... */
			r_timestamp(&action0);
			r_statetrans(&action0, &newaction,
				     A(newaction.button & MOUSE_BUTTON1DOWN,
				       action0.button & MOUSE_BUTTON3DOWN));

			/* Send middle down */
			newaction.button = MOUSE_BUTTON2DOWN;
			r_click(&newaction);

			/* Send middle up */
			r_timestamp(&newaction);
			newaction.obutton = newaction.button;
			newaction.button = action0.button;
			r_click(&newaction);
		    }
		}
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

	    if ((rodent.flags & VirtualScroll) || (rodent.flags & HVirtualScroll)) {
		/*
		 * If *only* the middle button is pressed AND we are moving
		 * the stick/trackpoint/nipple, scroll!
		 */
		if (scroll_state == SCROLL_PREPARE) {
			/* Middle button down, waiting for movement threshold */
			if (action2.dy || action2.dx) {
				if (rodent.flags & VirtualScroll) {
					scroll_movement += action2.dy;
					if (scroll_movement < -rodent.scrollthreshold) {
						scroll_state = SCROLL_SCROLLING;
					} else if (scroll_movement > rodent.scrollthreshold) {
						scroll_state = SCROLL_SCROLLING;
					}
				}
				if (rodent.flags & HVirtualScroll) {
					hscroll_movement += action2.dx;
					if (hscroll_movement < -rodent.scrollthreshold) {
						scroll_state = SCROLL_SCROLLING;
					} else if (hscroll_movement > rodent.scrollthreshold) {
						scroll_state = SCROLL_SCROLLING;
					}
				}
				if (scroll_state == SCROLL_SCROLLING) scroll_movement = hscroll_movement = 0;
			}
		} else if (scroll_state == SCROLL_SCROLLING) {
			 if (rodent.flags & VirtualScroll) {
				 scroll_movement += action2.dy;
				 debug("SCROLL: %d", scroll_movement);
			    if (scroll_movement < -rodent.scrollspeed) {
				/* Scroll down */
				action2.dz = -1;
				scroll_movement = 0;
			    }
			    else if (scroll_movement > rodent.scrollspeed) {
				/* Scroll up */
				action2.dz = 1;
				scroll_movement = 0;
			    }
			 }
			 if (rodent.flags & HVirtualScroll) {
				 hscroll_movement += action2.dx;
				 debug("HORIZONTAL SCROLL: %d", hscroll_movement);

				 if (hscroll_movement < -rodent.scrollspeed) {
					 action2.dz = -2;
					 hscroll_movement = 0;
				 }
				 else if (hscroll_movement > rodent.scrollspeed) {
					 action2.dz = 2;
					 hscroll_movement = 0;
				 }
			 }

		    /* Don't move while scrolling */
		    action2.dx = action2.dy = 0;
		}
	    }

	    if (drift_terminate) {
		if ((flags & MOUSE_POSCHANGED) == 0 || action.dz || action2.dz)
		    drift_last_activity = drift_current_ts;
		else {
		    /* X or/and Y movement only - possibly drift */
		    tssub(&drift_current_ts, &drift_last_activity, &drift_tmp);
		    if (tscmp(&drift_tmp, &drift_after_ts, >)) {
			tssub(&drift_current_ts, &drift_since, &drift_tmp);
			if (tscmp(&drift_tmp, &drift_time_ts, <)) {
			    drift_last.x += action2.dx;
			    drift_last.y += action2.dy;
			} else {
			    /* discard old accumulated steps (drift) */
			    if (tscmp(&drift_tmp, &drift_2time_ts, >))
				drift_previous.x = drift_previous.y = 0;
			    else
				drift_previous = drift_last;
			    drift_last.x = action2.dx;
			    drift_last.y = action2.dy;
			    drift_since = drift_current_ts;
			}
			if (abs(drift_last.x) + abs(drift_last.y)
			  > drift_distance) {
			    /* real movement, pass all accumulated steps */
			    action2.dx = drift_previous.x + drift_last.x;
			    action2.dy = drift_previous.y + drift_last.y;
			    /* and reset accumulators */
			    tsclr(&drift_since);
			    drift_last.x = drift_last.y = 0;
			    /* drift_previous will be cleared at next movement*/
			    drift_last_activity = drift_current_ts;
			} else {
			    continue;   /* don't pass current movement to
					 * console driver */
			}
		    }
		}
	    }

	    if (extioctl) {
		/* Defer clicks until we aren't VirtualScroll'ing. */
		if (scroll_state == SCROLL_NOTSCROLLING)
		    r_click(&action2);

		if (action2.flags & MOUSE_POSCHANGED) {
		    mouse.operation = MOUSE_MOTION_EVENT;
		    mouse.u.data.buttons = action2.button;
		    if (rodent.flags & ExponentialAcc) {
			expoacc(action2.dx, action2.dy,
			    &mouse.u.data.x, &mouse.u.data.y);
		    }
		    else {
			linacc(action2.dx, action2.dy,
			    &mouse.u.data.x, &mouse.u.data.y);
		    }
		    mouse.u.data.z = action2.dz;
		    if (debug < 2)
			if (!paused)
				ioctl(rodent.cfd, CONS_MOUSECTL, &mouse);
		}
	    } else {
		mouse.operation = MOUSE_ACTION;
		mouse.u.data.buttons = action2.button;
		if (rodent.flags & ExponentialAcc) {
		    expoacc(action2.dx, action2.dy,
			&mouse.u.data.x, &mouse.u.data.y);
		}
		else {
		    linacc(action2.dx, action2.dy,
			&mouse.u.data.x, &mouse.u.data.y);
		}
		mouse.u.data.z = action2.dz;
		if (debug < 2)
		    if (!paused)
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

		if (extioctl) {
		    r_click(&action2);
		} else {
		    mouse.operation = MOUSE_ACTION;
		    mouse.u.data.buttons = action2.button;
		    mouse.u.data.x = mouse.u.data.y = mouse.u.data.z = 0;
		    if (debug < 2)
			if (!paused)
			    ioctl(rodent.cfd, CONS_MOUSECTL, &mouse);
		}
	    }
	}
    }
    /* NOT REACHED */
}

static void
hup(__unused int sig)
{
    longjmp(env, 1);
}

static void
cleanup(__unused int sig)
{
    exit(0);
}

static void
pause_mouse(__unused int sig)
{
    paused = !paused;
}

/**
 ** usage
 **
 ** Complain, and free the CPU for more worthy tasks
 **/
static void
usage(void)
{
    fprintf(stderr, "%s\n%s\n%s\n%s\n%s\n",
	"usage: moused [-cdf] [-I file]",
	"              [-VH [-U threshold]] [-a X[,Y]] [-C threshold] [-m N=M] [-w N]",
	"              [-z N] [-3 [-E timeout]]",
	"              [-T distance[,time[,after]]] -p <port>",
	"       moused [-d] -i <port|type|model|all> -p <port>");
    exit(1);
}

/*
 * Output an error message to syslog or stderr as appropriate. If
 * `errnum' is non-zero, append its string form to the message.
 */
static void
log_or_warn(int log_pri, int errnum, const char *fmt, ...)
{
	va_list ap;
	char buf[256];

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (errnum) {
		strlcat(buf, ": ", sizeof(buf));
		strlcat(buf, strerror(errnum), sizeof(buf));
	}

	if (background)
		syslog(log_pri, "%s", buf);
	else
		warnx("%s", buf);
}

static inline int
bit_find(bitstr_t *array, int start, int stop)
{
    int res;

    bit_ffs_at(array, start, stop - start, &res);
    return (res != -1);
}

/* Derived from EvdevProbe() function of xf86-input-evdev driver */
static int
r_identify_evdev(void)
{
    int has_keys = bit_find(rodent.key_bits, 0, BTN_MISC);
    int has_buttons = bit_find(rodent.key_bits, BTN_MISC, BTN_JOYSTICK);
    int has_lmr = bit_find(rodent.key_bits, BTN_LEFT, BTN_MIDDLE + 1);
    int has_rel_axes = bit_find(rodent.rel_bits, 0, REL_CNT);
    int has_abs_axes = bit_find(rodent.abs_bits, 0, ABS_CNT);
    int has_mt = bit_find(rodent.abs_bits, ABS_MT_SLOT, ABS_CNT);

    if (has_abs_axes) {
        if (has_mt && !has_buttons) {
            /* TBD:Improve joystick detection */
            if (bit_test(rodent.key_bits, BTN_JOYSTICK)) {
                return MOUSE_PROTO_JOYSTICK;
            } else {
                has_buttons = TRUE;
            }
        }

        if (bit_test(rodent.abs_bits, ABS_X) &&
            bit_test(rodent.abs_bits, ABS_Y)) {
            if (bit_test(rodent.key_bits, BTN_TOOL_PEN) ||
                bit_test(rodent.key_bits, BTN_STYLUS) ||
                bit_test(rodent.key_bits, BTN_STYLUS2)) {
                    return MOUSE_PROTO_TABLET;
            } else if (bit_test(rodent.abs_bits, ABS_PRESSURE) ||
                       bit_test(rodent.key_bits, BTN_TOUCH)) {
                if (has_lmr || bit_test(rodent.key_bits, BTN_TOOL_FINGER)) {
                    return MOUSE_PROTO_TOUCHPAD;
                } else {
                    return MOUSE_PROTO_TOUCHSCREEN;
                }
            } else if (!(bit_test(rodent.rel_bits, REL_X) &&
                         bit_test(rodent.rel_bits, REL_Y)) &&
                       has_lmr) {
                /* some touchscreens use BTN_LEFT rather than BTN_TOUCH */
                return MOUSE_PROTO_TOUCHSCREEN;
            }
        }
    }

    if (has_keys)
        return MOUSE_PROTO_KEYBOARD;
    else if (has_rel_axes || has_abs_axes || has_buttons)
        return MOUSE_PROTO_MOUSE;

    return MOUSE_PROTO_UNKNOWN;
}

static int
r_identify(void)
{
	/* maybe this is a evdev mouse... */
	if (ioctl(rodent.mfd, EVIOCGNAME(
		  sizeof(rodent.model) - 1), rodent.model) < 0 ||
	    ioctl(rodent.mfd, EVIOCGBIT(EV_REL,
	          sizeof(rodent.rel_bits)), rodent.rel_bits) < 0 ||
	    ioctl(rodent.mfd, EVIOCGBIT(EV_ABS,
	          sizeof(rodent.abs_bits)), rodent.abs_bits) < 0 ||
	    ioctl(rodent.mfd, EVIOCGBIT(EV_KEY,
	          sizeof(rodent.key_bits)), rodent.key_bits) < 0) {
		return (MOUSE_PROTO_UNKNOWN);
	}

	rodent.rtype = r_identify_evdev();

	switch (rodent.rtype) {
	case MOUSE_PROTO_MOUSE:
	case MOUSE_PROTO_TOUCHPAD:
		break;
	default:
		debug("unsupported evdev type: %s", r_name(rodent.rtype));
		return (MOUSE_PROTO_UNKNOWN);
	}

	return (rodent.rtype);
}

static const char *
r_name(int type)
{
    const char *unknown = "unknown";

    return (type == MOUSE_PROTO_UNKNOWN ||
	type >= (int)(sizeof(rnames) / sizeof(rnames[0])) ?
	unknown : rnames[type]);
}

static void
r_init(void)
{
}

typedef struct packet {
    int     x;
    int     y;
} packet_t;

#define PACKETQUEUE	10
#define QUEUE_CURSOR(x)	(x + PACKETQUEUE) % PACKETQUEUE

typedef struct smoother {
    packet_t    queue[PACKETQUEUE];
    int         queue_len;
    int         queue_cursor;
    int         start_x;
    int         start_y;
    int         avg_dx;
    int         avg_dy;
    int         squelch_x;
    int         squelch_y;
    int         is_fuzzy;
    int         active;
} smoother_t;

typedef struct gesture {
    int                 window_min;
    int                 fingers_nb;
    int                 tap_button;
    int                 in_taphold;
    int                 in_vscroll;
    int                 zmax;           /* maximum pressure value */
    struct timeval      taptimeout;     /* tap timeout for touchpads */
} gesture_t;

static int
r_protocol(int rtype, struct input_event *ie, mousestatus_t *act)
{
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
	static int rel_x, rel_y, rel_z, rel_w;
	static int abs_x, abs_y, abs_p, abs_w;
	static int oabs_x, oabs_y;
	static int button, nfingers;
	static bool touch, otouch;

	debug("received event 0x%x, 0x%x, %d", ie->type, ie->code, ie->value);

	switch (ie->type) {
	case EV_REL:
		switch (ie->code) {
		case REL_X:
			rel_x = ie->value;
			break;
		case REL_Y:
			rel_y = ie->value;
			break;
		case REL_WHEEL:
			rel_z = ie->value;
			break;
		case REL_HWHEEL:
			rel_w = ie->value;
			break;
		}
		break;
	case EV_ABS:
		switch (ie->code) {
		case ABS_X:
			abs_x = ie->value;
			break;
		case ABS_Y:
			abs_y = ie->value;
			break;
		case ABS_PRESSURE:
			abs_p = ie->value;
			break;
		case ABS_TOOL_WIDTH:
			abs_w = ie->value;
			break;
		}
		break;
	case EV_KEY:
		switch (ie->code) {
		case BTN_TOUCH:
			touch = ie->value != 0;
			break;
		case BTN_TOOL_FINGER:
			nfingers = ie->value != 0 ? 1 : nfingers;
			break;
		case BTN_TOOL_DOUBLETAP:
			nfingers = ie->value != 0 ? 2 : nfingers;
			break;
		case BTN_TOOL_TRIPLETAP:
			nfingers = ie->value != 0 ? 3 : nfingers;
			break;
		case BTN_TOOL_QUADTAP:
			nfingers = ie->value != 0 ? 4 : nfingers;
			break;
		case BTN_TOOL_QUINTTAP:
			nfingers = ie->value != 0 ? 5 : nfingers;
			break;
		case BTN_LEFT ... BTN_LEFT + 7:
			button &= ~(1 << (ie->code - BTN_LEFT));
			button |= ((!!ie->value) << (ie->code - BTN_LEFT));
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

	act->obutton = act->button;

	act->button = butmapev[button & MOUSE_SYS_STDBUTTONS];
	act->button |= (button & MOUSE_SYS_EXTBUTTONS);
	switch (rtype) {
	case MOUSE_PROTO_MOUSE:
		debug("assembled full packet %d,%d,%d", rel_x, rel_y, rel_z);
		act->dx = rel_x;
		act->dy = rel_y;
		act->dz = rel_z;
		break;
	case MOUSE_PROTO_TOUCHPAD:
		debug("assembled full packet %d,%d,%d,%d", abs_x, abs_y, abs_p,
		    abs_w);
		if (touch && otouch) {
			act->dx = abs_x - oabs_x;
			act->dy = abs_y - oabs_y;
		}
		oabs_x = abs_x;
		oabs_y = abs_y;
		otouch = touch;
	}

	/*
	 * We don't reset pBufP here yet, as there may be an additional data
	 * byte in some protocols. See above.
	 */

	/* has something changed? */
	act->flags = ((act->dx || act->dy || act->dz) ? MOUSE_POSCHANGED : 0)
	    | (act->obutton ^ act->button);

	return (act->flags);
}

static int
r_statetrans(mousestatus_t *a1, mousestatus_t *a2, int trans)
{
    int changed;
    int flags;

    a2->dx = a1->dx;
    a2->dy = a1->dy;
    a2->dz = a1->dz;
    a2->obutton = a2->button;
    a2->button = a1->button;
    a2->flags = a1->flags;
    changed = FALSE;

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
			changed = TRUE;
		} else
			a2->dx = a2->dy = 0;
	} else
		mouse_move_delayed = 0;
	if (mouse_button_state != states[mouse_button_state].s[trans])
		changed = TRUE;
	if (changed)
		clock_gettime(CLOCK_MONOTONIC_FAST, &mouse_button_state_ts);
	mouse_button_state = states[mouse_button_state].s[trans];
	a2->button &=
	    ~(MOUSE_BUTTON1DOWN | MOUSE_BUTTON2DOWN | MOUSE_BUTTON3DOWN);
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

static int
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
	    return (FALSE);
	lbutton = atoi(s);

	arg = skipspace(++arg);
	s = arg;
	while (isdigit(*arg))
	    ++arg;
	if ((arg <= s) || (!isspace(*arg) && (*arg != '\0')))
	    return (FALSE);
	pbutton = atoi(s);

	if ((lbutton <= 0) || (lbutton > MOUSE_MAXBUTTON))
	    return (FALSE);
	if ((pbutton <= 0) || (pbutton > MOUSE_MAXBUTTON))
	    return (FALSE);
	p2l[pbutton - 1] = 1 << (lbutton - 1);
	mstate[lbutton - 1] = &bstate[pbutton - 1];
    }

    return (TRUE);
}

static void
r_map(mousestatus_t *act1, mousestatus_t *act2)
{
    register int pb;
    register int pbuttons;
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

    act2->flags = ((act2->dx || act2->dy || act2->dz) ? MOUSE_POSCHANGED : 0)
	| (act2->obutton ^ act2->button);
}

static void
r_timestamp(mousestatus_t *act)
{
    struct timespec ts;
    struct timespec ts1;
    struct timespec ts2;
    struct timespec ts3;
    int button;
    int mask;
    int i;

    mask = act->flags & MOUSE_BUTTONS;
#if 0
    if (mask == 0)
	return;
#endif

    clock_gettime(CLOCK_MONOTONIC_FAST, &ts1);
    drift_current_ts = ts1;

    /* double click threshold */
    ts2.tv_sec = rodent.clickthreshold / 1000;
    ts2.tv_nsec = (rodent.clickthreshold % 1000) * 1000000;
    tssub(&ts1, &ts2, &ts);
    debug("ts:  %jd %ld", (intmax_t)ts.tv_sec, ts.tv_nsec);

    /* 3 button emulation timeout */
    ts2.tv_sec = rodent.button2timeout / 1000;
    ts2.tv_nsec = (rodent.button2timeout % 1000) * 1000000;
    tssub(&ts1, &ts2, &ts3);

    button = MOUSE_BUTTON1DOWN;
    for (i = 0; (i < MOUSE_MAXBUTTON) && (mask != 0); ++i) {
	if (mask & 1) {
	    if (act->button & button) {
		/* the button is down */
		debug("  :  %jd %ld",
		    (intmax_t)bstate[i].ts.tv_sec, bstate[i].ts.tv_nsec);
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
		if (tscmp(&ts3, &bstate[i].ts, >)) {
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

static int
r_timeout(void)
{
    struct timespec ts;
    struct timespec ts1;
    struct timespec ts2;

    if (states[mouse_button_state].timeout)
	return (TRUE);
    clock_gettime(CLOCK_MONOTONIC_FAST, &ts1);
    ts2.tv_sec = rodent.button2timeout / 1000;
    ts2.tv_nsec = (rodent.button2timeout % 1000) * 1000000;
    tssub(&ts1, &ts2, &ts);
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
	    if (debug < 2)
		if (!paused)
		    ioctl(rodent.cfd, CONS_MOUSECTL, &mouse);
	    debug("button %d  count %d", i + 1, mouse.u.event.value);
	}
	button <<= 1;
	mask >>= 1;
    }
}
