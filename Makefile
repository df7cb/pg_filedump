# View README.pg_filedump first

CC=gcc
CFLAGS=-g -O -Wall -Wmissing-prototypes -Wmissing-declarations

INCLUDE=/usr/include/pgsql/server

# PGSQL MUST POINT TO pgsql SOURCE DIRECTORY
PGSQL=../../../../postgres/pgsql

CRC_SRC=${PGSQL}/src/backend/utils/hash
CRC_INCLUDE=${PGSQL}/src

all: pg_filedump

pg_filedump: pg_filedump.o pg_crc.o 
	${CC} ${CFLAGS} -o pg_filedump pg_filedump.o pg_crc.o

pg_filedump.o: pg_filedump.c
	${CC} ${CFLAGS} -I${INCLUDE} pg_filedump.c -c

pg_crc.o: ${CRC_SRC}/pg_crc.c
	${CC} ${CFLAGS} -I${CRC_INCLUDE} -I${INCLUDE} ${CRC_SRC}/pg_crc.c -c 

clean:
	rm -rf *.o pg_filedump
