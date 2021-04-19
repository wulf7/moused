# $FreeBSD$

MK_DEBUG_FILES=	no

LOCALBASE?=	/usr/local
PREFIX?=	${LOCALBASE}
BINDIR?=	${PREFIX}/sbin
MANDIR?=	${PREFIX}/man/man
CONFSDIR=       ${PREFIX}/etc/devd

PROG=		moused
SRCS=		moused.c quirks.c quirks.h util.c util.h util-list.c util-list.h
MAN=		moused.8
CONFS=		devd.conf
CONFSNAME=	moused.conf

LDADD=		-lm -lutil

.include <bsd.prog.mk>

install:	installconfig
