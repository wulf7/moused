# $FreeBSD$

MK_DEBUG_FILES=	no

LOCALBASE?=	/usr/local
PREFIX?=	${LOCALBASE}
BINDIR?=	${PREFIX}/sbin
MANDIR?=	${PREFIX}/man/man
DEVDDIR=	${PREFIX}/etc/devd
CONFSDIR=	${PREFIX}/etc
FILESDIR=	${PREFIX}/share/${PROG}

QUIRKS=	10-generic-keyboard.quirks \
	10-generic-mouse.quirks \
	10-generic-trackball.quirks \
	30-vendor-a4tech.quirks \
	30-vendor-aiptek.quirks \
	30-vendor-alps.quirks \
	30-vendor-contour.quirks \
	30-vendor-cypress.quirks \
	30-vendor-elantech.quirks \
	30-vendor-glorious.quirks \
	30-vendor-huion.quirks \
	30-vendor-ibm.quirks \
	30-vendor-ite.quirks \
	30-vendor-kensington.quirks \
	30-vendor-logitech.quirks \
	30-vendor-madcatz.quirks \
	30-vendor-microsoft.quirks \
	30-vendor-oracle.quirks \
	30-vendor-qemu.quirks \
	30-vendor-razer.quirks \
	30-vendor-synaptics.quirks \
	30-vendor-trust.quirks \
	30-vendor-uniwill.quirks \
	30-vendor-vmware.quirks \
	30-vendor-wacom.quirks \
	50-system-acer.quirks \
	50-system-apple.quirks \
	50-system-asus.quirks \
	50-system-chicony.quirks \
	50-system-chuwi.quirks \
	50-system-cyborg.quirks \
	50-system-dell.quirks \
	50-system-framework.quirks \
	50-system-gigabyte.quirks \
	50-system-google.quirks \
	50-system-gpd.quirks \
	50-system-graviton.quirks \
	50-system-hp.quirks \
	50-system-huawei.quirks \
	50-system-lenovo.quirks \
	50-system-pine64.quirks \
	50-system-sony.quirks \
	50-system-starlabs.quirks \
	50-system-system76.quirks \
	50-system-toshiba.quirks \
	50-system-vaio.quirks

PROG=		moused
SRCS=		moused.c quirks.c quirks.h util.c util.h util-list.c util-list.h
MAN=		moused.8
CONFGROUPS=	DEVD MOUSED
DEVD=		devd.conf
DEVDNAME=	moused.conf
CONFS=		moused.conf
FILES=		${QUIRKS:S|^|quirks/|}

CFLAGS+=	-DCONFSDIR=\"${CONFSDIR}\" -DQUIRKSDIR=\"${FILESDIR}\"
LDADD=		-lm -lutil

.include <bsd.prog.mk>

install:	installconfig
