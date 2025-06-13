# $FreeBSD$

MK_DEBUG_FILES=	no

PROG=		moused

LOCALBASE?=	/usr/local
PREFIX?=	${LOCALBASE}

QUIRKS=	10-generic-touchpad.quirks

SRCS=		moused.c \
		event-names.h \
		quirks.c \
		quirks.h \
		util.c \
		util.h \
		util-evdev.c \
		util-evdev.h \
		util-list.c \
		util-list.h

CFLAGS+=	-DCONFDIR=\"${MOUSEDDIR}\" -DQUIRKSDIR=\"${FILESDIR}\"
LDADD=		-lm -lutil
BINDIR?=	${PREFIX}/sbin

MAN=		moused.8 \
		moused.conf.5
MANDIR?=	${PREFIX}/share/man/man

CONFGROUPS=	MOUSED RC

MOUSED=		moused.conf
MOUSEDDIR=	${PREFIX}/etc

RC=		moused.rc
RCNAME=		evdev_moused
RCDIR=		${PREFIX}/etc/rc.d
RCMODE=		${BINMODE}

FILES=		${QUIRKS:S|^|quirks/|}
FILESDIR=	${PREFIX}/share/${PROG}

.include <bsd.prog.mk>

install:	installconfig
