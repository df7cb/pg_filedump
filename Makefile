# View README.pg_filedump first

# note this must match version macros in pg_filedump.h
FD_VERSION=10.1

PROGRAM = pg_filedump
OBJS = pg_filedump.o decode.o stringinfo.o pg_lzcompress.o

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# avoid linking against all libs that the server links against (xml, selinux, ...)
LIBS = $(libpq_pgport)

DISTFILES= README.pg_filedump Makefile Makefile.contrib \
	pg_filedump.h pg_filedump.c decode.h decode.c stringinfo.c pg_lzcompress.c

dist:
	rm -rf pg_filedump-${FD_VERSION} pg_filedump-${FD_VERSION}.tar.gz
	mkdir pg_filedump-${FD_VERSION}
	cp -p ${DISTFILES} pg_filedump-${FD_VERSION}
	tar cfz pg_filedump-${FD_VERSION}.tar.gz pg_filedump-${FD_VERSION}
	rm -rf pg_filedump-${FD_VERSION}

