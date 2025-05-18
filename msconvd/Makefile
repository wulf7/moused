MK_DEBUG_FILES=	no

PROG=           msconvd

LOCALBASE?=     /usr/local
PREFIX?=        ${LOCALBASE}

SRCS=           ${PROG}.c
LDADD=		-lutil
BINDIR?=        ${PREFIX}/sbin

MAN=            ${PROG}.8
MANDIR?=        ${PREFIX}/share/man/man

.include <bsd.prog.mk>
