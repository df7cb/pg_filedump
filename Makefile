# View README.pg_filedump first

# note this must match version macros in pg_filedump.h
FD_VERSION=9.2.0

CC=gcc
CFLAGS=-g -O -Wall -Wmissing-prototypes -Wmissing-declarations

# If working with a PG source directory, point PGSQL_INCLUDE_DIR to its
# src/include subdirectory.  If working with an installed tree, point to
# the server include subdirectory, eg /usr/local/include/postgresql/server
PGSQL_INCLUDE_DIR=../../pgsql/src/include


DISTFILES= README.pg_filedump Makefile Makefile.contrib \
	pg_filedump.h pg_filedump.c

all: pg_filedump

pg_filedump: pg_filedump.o
	${CC} ${CFLAGS} -o pg_filedump pg_filedump.o

pg_filedump.o: pg_filedump.c
	${CC} ${CFLAGS} -I${PGSQL_INCLUDE_DIR} pg_filedump.c -c

dist:
	rm -rf pg_filedump-${FD_VERSION} pg_filedump-${FD_VERSION}.tar.gz
	mkdir pg_filedump-${FD_VERSION}
	cp -p ${DISTFILES} pg_filedump-${FD_VERSION}
	tar cfz pg_filedump-${FD_VERSION}.tar.gz pg_filedump-${FD_VERSION}
	rm -rf pg_filedump-${FD_VERSION}

clean:
	rm -f *.o pg_filedump
