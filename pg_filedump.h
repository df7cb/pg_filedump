/*
 * pg_filedump.h - PostgreSQL file dump utility for dumping and
 *                 formatting heap (data), index and control files.
 *
 * Copyright (c) 2002-2010 Red Hat, Inc.
 * Copyright (c) 2011-2012, PostgreSQL Global Development Group
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Original Author: Patrick Macdonald <patrickm@redhat.com>
 */

#define FD_VERSION	"9.2.0"			/* version ID of pg_filedump */
#define FD_PG_VERSION	"PostgreSQL 9.2.x"	/* PG version it works with */

#include "postgres.h"

#include <time.h>
#include <ctype.h>

#include "access/gin_private.h"
#include "access/gist.h"
#include "access/hash.h"
#include "access/htup.h"
#include "access/itup.h"
#include "access/nbtree.h"
#include "access/spgist_private.h"
#include "catalog/pg_control.h"
#include "storage/bufpage.h"

// Options for Block formatting operations
static unsigned int blockOptions = 0;
typedef enum
{
  BLOCK_ABSOLUTE = 0x00000001,	// -a: Absolute (vs Relative) addressing
  BLOCK_BINARY = 0x00000002,	// -b: Binary dump of block
  BLOCK_FORMAT = 0x00000004,	// -f: Formatted dump of blocks / control file
  BLOCK_FORCED = 0x00000008,	// -S: Block size forced
  BLOCK_NO_INTR = 0x00000010,	// -d: Dump straight blocks
  BLOCK_RANGE = 0x00000020	// -R: Specific block range to dump
}
blockSwitches;

static int blockStart = -1;	// -R [start]: Block range start
static int blockEnd = -1;	// -R [end]: Block range end

// Options for Item formatting operations
static unsigned int itemOptions = 0;
typedef enum
{
  ITEM_DETAIL = 0x00000001,	// -i: Display interpreted items
  ITEM_HEAP = 0x00000002,	// -y: Blocks contain HeapTuple items
  ITEM_INDEX = 0x00000004,	// -x: Blocks contain IndexTuple items
  ITEM_SPG_INNER = 0x00000008,	// Blocks contain SpGistInnerTuple items
  ITEM_SPG_LEAF = 0x00000010	// Blocks contain SpGistLeafTuple items
}
itemSwitches;

// Options for Control File formatting operations
static unsigned int controlOptions = 0;
typedef enum
{
  CONTROL_DUMP = 0x00000001,	// -c: Dump control file
  CONTROL_FORMAT = BLOCK_FORMAT,	// -f: Formatted dump of control file
  CONTROL_FORCED = BLOCK_FORCED	// -S: Block size forced
}
controlSwitches;

// Possible value types for the Special Section
typedef enum
{
  SPEC_SECT_NONE,		// No special section on block
  SPEC_SECT_SEQUENCE,		// Sequence info in special section
  SPEC_SECT_INDEX_BTREE,	// BTree index info in special section
  SPEC_SECT_INDEX_HASH,		// Hash index info in special section
  SPEC_SECT_INDEX_GIST,		// GIST index info in special section
  SPEC_SECT_INDEX_GIN,		// GIN index info in special section
  SPEC_SECT_INDEX_SPGIST,	// SP-GIST index info in special section
  SPEC_SECT_ERROR_UNKNOWN,	// Unknown error
  SPEC_SECT_ERROR_BOUNDARY	// Boundary error
}
specialSectionTypes;
static unsigned int specialType = SPEC_SECT_NONE;

// Possible return codes from option validation routine.
// pg_filedump doesn't do much with them now but maybe in
// the future...
typedef enum
{
  OPT_RC_VALID,			// All options are valid
  OPT_RC_INVALID,		// Improper option string
  OPT_RC_FILE,			// File problems
  OPT_RC_DUPLICATE,		// Duplicate option encountered
  OPT_RC_COPYRIGHT		// Copyright should be displayed
}
optionReturnCodes;

// Simple macro to check for duplicate options and then set
// an option flag for later consumption
#define SET_OPTION(_x,_y,_z) if (_x & _y)               \
                               {                        \
                                 rc = OPT_RC_DUPLICATE; \
                                 duplicateSwitch = _z;  \
                               }                        \
                             else                       \
                               _x |= _y;

#define SEQUENCE_MAGIC 0x1717	// PostgreSQL defined magic number
#define EOF_ENCOUNTERED (-1)	// Indicator for partial read
#define BYTES_PER_LINE 16	// Format the binary 16 bytes per line
