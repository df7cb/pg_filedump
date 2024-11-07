# pg_filedump - Display formatted contents of a PostgreSQL heap, index, or control file

Copyright (c) 2002-2010 Red Hat, Inc.

Copyright (c) 2011-2024, PostgreSQL Global Development Group

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

Original Author: Patrick Macdonald <patrickm@redhat.com>


## Overview:

pg_filedump is a utility to format PostgreSQL heap/index/control files
into a human-readable form.  You can format/dump the files several ways,
as listed in the Invocation section, as well as dumping straight binary.

The type of file (heap/index) can usually be determined automatically
by the content of the blocks within the file.  However, to format a
pg_control file you must use the -c option.

The default is to format the entire file using the block size listed in
block 0 and display block relative addresses.  These defaults can be
modified using run-time options.

Some options may seem strange but they're there for a reason.  For
example, block size.  It's there because if the header of block 0 is
corrupt, you need a method of forcing a block size.


## Compile/Installation:

To compile pg_filedump, you will need to have a properly configured
PostgreSQL source tree or the devel packages (with include files)
of the appropriate PostgreSQL major version.

```
make PG_CONFIG=/path/to/postgresql/bin/pg_config
make install PG_CONFIG=/path/to/postgresql/bin/pg_config
```


## Invocation:

```
Usage: pg_filedump [-abcdfhikxy] [-R startblock [endblock]] [-D attrlist] [-S blocksize] [-s segsize] [-n segnumber] file

Display formatted contents of a PostgreSQL heap/index/control file
Defaults are: relative addressing, range of the entire file, block
               size as listed on block 0 in the file

The following options are valid for heap and index files:
  -a  Display absolute addresses when formatting (Block header
      information is always block relative)
  -b  Display binary block images within a range (Option will turn
      off all formatting options)
  -d  Display formatted block content dump (Option will turn off
      all other formatting options)
  -D  Decode tuples using given comma separated list of types
      Supported types:
        bigint bigserial bool char charN date float float4 float8 int
        json macaddr name numeric oid real serial smallint smallserial text
        time timestamp timestamptz timetz uuid varchar varcharN xid xml
      ~ ignores all attributes left in a tuple
  -f  Display formatted block content dump along with interpretation
  -h  Display this information
  -i  Display interpreted item details
  -k  Verify block checksums
  -o  Do not dump old values.
  -R  Display specific block ranges within the file (Blocks are
      indexed from 0)
        [startblock]: block to start at
        [endblock]: block to end at
      A startblock without an endblock will format the single block
  -s  Force segment size to [segsize]
  -t  Dump TOAST files
  -v  Ouput additional information about TOAST relations
  -n  Force segment number to [segnumber]
  -S  Force block size to [blocksize]
  -x  Force interpreted formatting of block items as index items
  -y  Force interpreted formatting of block items as heap items

The following options are valid for control files:
  -c  Interpret the file listed as a control file
  -f  Display formatted content dump along with interpretation
  -S  Force block size to [blocksize]
Additional functions:
  -m  Interpret file as pg_filenode.map file and print contents (all
      other options will be ignored)

Report bugs to <pgsql-bugs@postgresql.org>
```

In most cases it's recommended to use the -i and -f options to get
the most useful dump output.
