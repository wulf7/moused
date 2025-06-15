MK_DEBUG_FILES=	no

PROG=           msconvd

LOCALBASE?=     /usr/local
PREFIX?=        ${LOCALBASE}

SRCS=           ${PROG}.c
LDADD=		-lutil
BINDIR?=        ${PREFIX}/sbin

MAN=            ${PROG}.8
MANDIR?=        ${PREFIX}/share/man/man

CONFS=		${PROG}.rc
CONFSNAME=	${PROG}
CONFSDIR=	${PREFIX}/etc/rc.d
CONFSMODE=	${BINMODE}

.include <bsd.prog.mk>

install:	installconfig
