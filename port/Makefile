PORTNAME=	moused
PORTVERSION=	20250615
CATEGORIES=	sysutils

MAINTAINER=	wulf@FreeBSD.org
COMMENT=	Moused with EVDEV support
WWW=		https://github.com/wulf7/moused

LICENSE=	BSD2CLAUSE

USES=		uidfix

USE_GITHUB=	yes
GH_ACCOUNT=	wulf7
GH_TAGNAME=	81ce31f600b3854a3179dcac18685d102ba4d8c8

post-install:
	${MV} ${STAGEDIR}${ETCDIR}.conf ${STAGEDIR}${ETCDIR}.conf.sample

.include <bsd.port.mk>
