PROGRAM = pg_filedump
OBJS = pg_filedump.o decode.o stringinfo.o
REGRESS = datatypes float numeric xml toast
TAP_TESTS = 1
EXTRA_CLEAN = *.heap $(wildcard [1-9]???[0-9]) # testsuite leftovers

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# make regression tests find pg_filedump (srcdir for build-time testing, bindir for later installcheck)
PATH += :$(srcdir):$(shell $(PG_CONFIG) --bindir)

# avoid linking against all libs that the server links against (xml, selinux, ...)
ifneq ($(findstring -llz4,$(LIBS)),)
       LIBS = -L$(pkglibdir) -lpgcommon -lpgport -llz4
else
       LIBS = -L$(pkglibdir) -lpgcommon -lpgport
endif
