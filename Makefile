# View README.pg_filedump first

# note this must match version macros in pg_filedump.h
FD_VERSION=9.6.0

# If working with a PG source directory, point PGSQL_INCLUDE_DIR to its
# src/include subdirectory.  If working with an installed tree, point to
# the server include subdirectory, eg /usr/local/include/postgresql/server
PG_CONFIG=pg_config
PGSQL_CFLAGS=$(shell $(PG_CONFIG) --cflags)
PGSQL_INCLUDE_DIR=$(shell $(PG_CONFIG) --includedir-server)
PGSQL_LDFLAGS=$(shell $(PG_CONFIG) --ldflags)
PGSQL_LIB_DIR=$(shell $(PG_CONFIG) --libdir)
PGSQL_BIN_DIR=$(shell $(PG_CONFIG) --bindir)

DISTFILES= README.pg_filedump Makefile Makefile.contrib \
	pg_filedump.h pg_filedump.c decode.h decode.c stringinfo.c pg_lzcompress.c

all: pg_filedump

pg_filedump: pg_filedump.o decode.o stringinfo.o pg_lzcompress.o
	${CC} ${PGSQL_LDFLAGS} ${LDFLAGS} -o pg_filedump pg_filedump.o decode.o stringinfo.o pg_lzcompress.o -L${PGSQL_LIB_DIR} -lpgport

pg_filedump.o: pg_filedump.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_filedump.c -c

decode.o: decode.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} decode.c -c

stringinfo.o: stringinfo.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} stringinfo.c -c

pg_lzcompress.o: pg_lzcompress.c
	${CC} ${PGSQL_CFLAGS} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_lzcompress.c -c

dist:
	rm -rf pg_filedump-${FD_VERSION} pg_filedump-${FD_VERSION}.tar.gz
	mkdir pg_filedump-${FD_VERSION}
	cp -p ${DISTFILES} pg_filedump-${FD_VERSION}
	tar cfz pg_filedump-${FD_VERSION}.tar.gz pg_filedump-${FD_VERSION}
	rm -rf pg_filedump-${FD_VERSION}

install: pg_filedump
	mkdir -p $(DESTDIR)$(PGSQL_BIN_DIR)
	install pg_filedump $(DESTDIR)$(PGSQL_BIN_DIR)

clean:
	rm -f *.o pg_filedump
