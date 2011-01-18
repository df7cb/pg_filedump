# View README.pg_filedump first

CC=gcc
CFLAGS=-g -O -Wall -Wmissing-prototypes -Wmissing-declarations

# PGSQL MUST POINT TO pgsql SOURCE DIRECTORY
PGSQL=../../pgsql

CRC_SRC_DIR=${PGSQL}/src/backend/utils/hash

INCLUDE_DIR=${PGSQL}/src/include

all: pg_filedump

pg_filedump: pg_filedump.o pg_crc.o 
	${CC} ${CFLAGS} -o pg_filedump pg_filedump.o pg_crc.o

pg_filedump.o: pg_filedump.c
	${CC} ${CFLAGS} -I${INCLUDE_DIR} pg_filedump.c -c

pg_crc.o: ${CRC_SRC_DIR}/pg_crc.c
	${CC} ${CFLAGS} -I${INCLUDE_DIR} ${CRC_SRC_DIR}/pg_crc.c -c 

clean:
	rm -f *.o pg_filedump
