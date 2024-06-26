# $FreeBSD$

MK_DEBUG_FILES=	no

PROG=		moused

LOCALBASE?=	/usr/local
PREFIX?=	${LOCALBASE}

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

SRCS=		moused.c quirks.c quirks.h util.c util.h util-list.c util-list.h
CFLAGS+=	-DCONFSDIR=\"${CONFSDIR}\" -DQUIRKSDIR=\"${FILESDIR}\"
LDADD=		-lm -lutil
BINDIR?=	${PREFIX}/sbin

MAN=		moused.8
MANDIR?=	${PREFIX}/share/man/man

CONFGROUPS=	DEVD MOUSED RC

DEVD=		devd.conf
DEVDNAME=	moused.conf
DEVDDIR=	${PREFIX}/etc/devd

CONFS=		moused.conf
CONFSDIR=	${PREFIX}/etc

RC=		moused.rc
RCNAME=		evdev_moused
RCDIR=		${PREFIX}/etc/rc.d
RCMODE=		${BINMODE}

FILES=		${QUIRKS:S|^|quirks/|}
FILESDIR=	${PREFIX}/share/${PROG}

.include <bsd.prog.mk>

install:	installconfig
