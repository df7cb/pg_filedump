# View README.pg_filedump first

# note this must match macro in pg_filedump.h
FD_VERSION=9.1.0

CC=gcc
CFLAGS=-g -O -Wall -Wmissing-prototypes -Wmissing-declarations

# PGSQL MUST POINT TO pgsql SOURCE DIRECTORY
PGSQL=../../pgsql

CRC_SRC_DIR=${PGSQL}/src/backend/utils/hash

INCLUDE_DIR=${PGSQL}/src/include

DISTFILES= README.pg_filedump Makefile Makefile.contrib \
	pg_filedump.h pg_filedump.c


all: pg_filedump

pg_filedump: pg_filedump.o pg_crc.o 
	${CC} ${CFLAGS} -o pg_filedump pg_filedump.o pg_crc.o

pg_filedump.o: pg_filedump.c
	${CC} ${CFLAGS} -I${INCLUDE_DIR} pg_filedump.c -c

pg_crc.o: ${CRC_SRC_DIR}/pg_crc.c
	${CC} ${CFLAGS} -I${INCLUDE_DIR} ${CRC_SRC_DIR}/pg_crc.c -c 

dist:
	rm -rf pg_filedump-${FD_VERSION} pg_filedump-${FD_VERSION}.tar.gz
	mkdir pg_filedump-${FD_VERSION}
	cp -p ${DISTFILES} pg_filedump-${FD_VERSION}
	tar cfz pg_filedump-${FD_VERSION}.tar.gz pg_filedump-${FD_VERSION}
	rm -rf pg_filedump-${FD_VERSION}

clean:
	rm -f *.o pg_filedump
