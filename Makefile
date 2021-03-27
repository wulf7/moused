# $FreeBSD$

MK_DEBUG_FILES=	no

LOCALBASE?=	/usr/local
PREFIX?=	${LOCALBASE}
BINDIR?=	${PREFIX}/sbin
MANDIR?=	${PREFIX}/man/man

PROG=		moused
MAN=		moused.8

LDADD=		-lm -lutil

.include <bsd.prog.mk>
