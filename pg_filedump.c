/*
 * pg_filedump.c - PostgreSQL file dump utility for dumping and
 *				   formatting heap (data), index and control files.
 *
 * Copyright (c) 2002-2010 Red Hat, Inc.
 * Copyright (c) 2011-2024, PostgreSQL Global Development Group
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

#include "pg_filedump.h"

#include <utils/pg_crc.h>

/*	checksum_impl.h uses Assert, which doesn't work outside the server */
#undef Assert
#define Assert(X)

#include "storage/checksum.h"
#include "storage/checksum_impl.h"
#include "decode.h"

/*
 * Global variables for ease of use mostly
 */
/*	Options for Block formatting operations */
unsigned int blockOptions = 0;

/* Segment-related options */
unsigned int segmentOptions = 0;

/* -R[start]:Block range start */
int	blockStart = -1;

/* -R[end]:Block range end */
int	blockEnd = -1;

/* Options for Item formatting operations */
unsigned int itemOptions = 0;

/* Options for Control File formatting operations */
unsigned int controlOptions = 0;

unsigned int specialType = SPEC_SECT_NONE;

static bool verbose = false;

/* File to dump or format */
FILE *fp = NULL;

/* File name for display */
char *fileName = NULL;

/* Current block size */
static unsigned int blockSize = 0;

/* Segment size in bytes */
static unsigned int segmentSize = RELSEG_SIZE * BLCKSZ;

/* Number of current segment */
static unsigned int segmentNumber = 0;

/* Offset of current block */
static unsigned int pageOffset = 0;

/* Number of bytes to format */
static unsigned int bytesToFormat = 0;

/* Block version number */
static unsigned int blockVersion = 0;

/* Flag to indicate pg_filenode.map file */
static bool isRelMapFile = false;

/* Program exit code */
static int	exitCode = 0;

/* Relmapper structs */
typedef struct RelMapping
{
  Oid     mapoid;     /* OID of a catalog */
  Oid     mapfilenode;  /* its filenode number */
} RelMapping;

/* crc and pad are ignored here, even though they are
 * present in the backend code.  We assume that anyone
 * seeking to inspect the contents of pg_filenode.map
 * probably have a corrupted or non-functional cluster */
typedef struct RelMapFile
{
  int32   magic;      /* always RELMAPPER_FILEMAGIC */
  int32   num_mappings; /* number of valid RelMapping entries */
  RelMapping  mappings[FLEXIBLE_ARRAY_MEMBER];
} RelMapFile;

/*
 * Function Prototypes
 */
unsigned int GetBlockSize(FILE *fp);

static void DisplayOptions(unsigned int validOptions);
static unsigned int ConsumeOptions(int numOptions, char **options);
static int	GetOptionValue(char *optionString);
static void FormatBlock(unsigned int blockOptions,
		unsigned int controlOptions,
		char *buffer,
		BlockNumber currentBlock,
		unsigned int blockSize,
		bool isToast,
		Oid toastOid,
		unsigned int toastExternalSize,
		char *toastValue,
		unsigned int *toastRead);
static unsigned int GetSpecialSectionType(char *buffer, Page page);
static bool IsBtreeMetaPage(Page page);
static void CreateDumpFileHeader(int numOptions, char **options);
static int	FormatHeader(char *buffer,
		Page page,
		BlockNumber blkno,
		bool isToast);
static void FormatItemBlock(char *buffer,
		Page page,
		bool isToast,
		Oid toastOid,
		unsigned int toastExternalSize,
		char *toastValue,
		unsigned int *toastRead);
static void FormatItem(char *buffer,
		unsigned int numBytes,
		unsigned int startIndex,
		unsigned int formatAs);
static void FormatSpecial(char *buffer);
static void FormatControl(char *buffer);
static void FormatBinary(char *buffer,
		unsigned int numBytes, unsigned int startIndex);
static void DumpBinaryBlock(char *buffer);
static int PrintRelMappings(void);


/* Send properly formed usage information to the user. */
static void
DisplayOptions(unsigned int validOptions)
{
	if (validOptions == OPT_RC_COPYRIGHT)
		printf
			("\nVersion %s (for %s)"
			 "\nCopyright (c) 2002-2010 Red Hat, Inc."
		  "\nCopyright (c) 2011-2024, PostgreSQL Global Development Group\n",
			 FD_VERSION, FD_PG_VERSION);

	printf
		("\nUsage: pg_filedump [-abcdfhikxy] [-R startblock [endblock]] [-D attrlist] [-S blocksize] [-s segsize] [-n segnumber] file\n\n"
		 "Display formatted contents of a PostgreSQL heap/index/control file\n"
		 "Defaults are: relative addressing, range of the entire file, block\n"
		 "               size as listed on block 0 in the file\n\n"
		 "The following options are valid for heap and index files:\n"
		 "  -a  Display absolute addresses when formatting (Block header\n"
		 "      information is always block relative)\n"
		 "  -b  Display binary block images within a range (Option will turn\n"
		 "      off all formatting options)\n"
		 "  -d  Display formatted block content dump (Option will turn off\n"
		 "      all other formatting options)\n"
		 "  -D  Decode tuples using given comma separated list of types\n"
		 "      Supported types:\n"
		 "        bigint bigserial bool char charN date float float4 float8 int\n"
		 "        json macaddr name numeric oid real serial smallint smallserial text\n"
		 "        time timestamp timestamptz timetz uuid varchar varcharN xid xml\n"
		 "      ~ ignores all attributes left in a tuple\n"
		 "  -f  Display formatted block content dump along with interpretation\n"
		 "  -h  Display this information\n"
		 "  -i  Display interpreted item details\n"
		 "  -k  Verify block checksums\n"
		 "  -o  Do not dump old values.\n"
		 "  -R  Display specific block ranges within the file (Blocks are\n"
		 "      indexed from 0)\n"
		 "        [startblock]: block to start at\n"
		 "        [endblock]: block to end at\n"
		 "      A startblock without an endblock will format the single block\n"
		 "  -s  Force segment size to [segsize]\n"
		 "  -t  Dump TOAST files\n"
		 "  -v  Ouput additional information about TOAST relations\n"
		 "  -n  Force segment number to [segnumber]\n"
		 "  -S  Force block size to [blocksize]\n"
		 "  -x  Force interpreted formatting of block items as index items\n"
		 "  -y  Force interpreted formatting of block items as heap items\n\n"
		 "The following options are valid for control files:\n"
		 "  -c  Interpret the file listed as a control file\n"
		 "  -f  Display formatted content dump along with interpretation\n"
		 "  -S  Force block size to [blocksize]\n"
		 "Additional functions:\n"
		 "  -m  Interpret file as pg_filenode.map file and print contents (all\n"
		 "      other options will be ignored)\n" 
		 "\nReport bugs to <pgsql-bugs@postgresql.org>\n");
}

/*
 * Determine segment number by segment file name. For instance, if file
 * name is /path/to/xxxx.7 procedure returns 7. Default return value is 0.
 */
static unsigned int
GetSegmentNumberFromFileName(const char *fileName)
{
	int			segnumOffset = strlen(fileName) - 1;

	if (segnumOffset < 0)
		return 0;

	while (isdigit(fileName[segnumOffset]))
	{
		segnumOffset--;
		if (segnumOffset < 0)
			return 0;
	}

	if (fileName[segnumOffset] != '.')
		return 0;

	return atoi(&fileName[segnumOffset + 1]);
}

/*	Iterate through the provided options and set the option flags.
 *	An error will result in a positive rc and will force a display
 *	of the usage information.  This routine returns enum
 *	optionReturnCode values. */
static unsigned int
ConsumeOptions(int numOptions, char **options)
{
	unsigned int rc = OPT_RC_VALID;
	unsigned int x;
	unsigned int optionStringLength;
	char	   *optionString;
	char		duplicateSwitch = 0x00;

	for (x = 1; x < numOptions; x++)
	{
		optionString = options[x];
		optionStringLength = strlen(optionString);

		/* Range is a special case where we have to consume the next 1 or 2
		 * parameters to mark the range start and end */
		if ((optionStringLength == 2) && (strcmp(optionString, "-R") == 0))
		{
			int			range = 0;

			SET_OPTION(blockOptions, BLOCK_RANGE, 'R');
			/* Only accept the range option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* Make sure there are options after the range identifier */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing range start identifier.\n");
				exitCode = 1;
				break;
			}

			/*
			 * Mark that we have the range and advance the option to what
			 * should be the range start. Check the value of the next
			 * parameter */
			optionString = options[++x];
			if ((range = GetOptionValue(optionString)) < 0)
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid range start identifier <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}

			/* The default is to dump only one block */
			blockStart = blockEnd = (unsigned int) range;

			/* We have our range start marker, check if there is an end
			 * marker on the option line.  Assume that the last option
			 * is the file we are dumping, so check if there are options
			 * range start marker and the file */
			if (x <= (numOptions - 3))
			{
				if ((range = GetOptionValue(options[x + 1])) >= 0)
				{
					/* End range must be => start range */
					if (blockStart <= range)
					{
						blockEnd = (unsigned int) range;
						x++;
					}
					else
					{
						rc = OPT_RC_INVALID;
						printf("Error: Requested block range start <%d> is "
							   "greater than end <%d>.\n", blockStart, range);
						exitCode = 1;
						break;
					}
				}
			}
		}
		/* Check for the special case where the user forces a block size
		 * instead of having the tool determine it.  This is useful if
		 * the header of block 0 is corrupt and gives a garbage block size */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-S") == 0))
		{
			int			localBlockSize;

			SET_OPTION(blockOptions, BLOCK_FORCED, 'S');
			/* Only accept the forced size option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -S is the block size */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing block size identifier.\n");
				break;
			}

			/* Next option encountered must be forced block size */
			optionString = options[++x];
			if ((localBlockSize = GetOptionValue(optionString)) > 0)
				blockSize = (unsigned int) localBlockSize;
			else
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid block size requested <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}
		}
		/* Check for the special case where the user forces a segment size. */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-s") == 0))
		{
			int			localSegmentSize;

			SET_OPTION(segmentOptions, SEGMENT_SIZE_FORCED, 's');
			/* Only accept the forced size option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -s is the segment size */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing segment size identifier.\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be forced segment size */
			optionString = options[++x];
			if ((localSegmentSize = GetOptionValue(optionString)) > 0)
				segmentSize = (unsigned int) localSegmentSize;
			else
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid segment size requested <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}
		}
		/* Check for the special case where the user forces tuples decoding. */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-D") == 0))
		{
			SET_OPTION(blockOptions, BLOCK_DECODE, 'D');
			/* Only accept the decode option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -D is attrubute types string */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing attribute types string.\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be attribute types string */
			optionString = options[++x];

			if (ParseAttributeTypesString(optionString) < 0)
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid attribute types string <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}
		}
		/* Check for the special case where the user forces a segment number
		 * instead of having the tool determine it by file name. */
		else if ((optionStringLength == 2)
				 && (strcmp(optionString, "-n") == 0))
		{
			int			localSegmentNumber;

			SET_OPTION(segmentOptions, SEGMENT_NUMBER_FORCED, 'n');
			/* Only accept the forced segment number option once */
			if (rc == OPT_RC_DUPLICATE)
				break;

			/* The token immediately following -n is the segment number */
			if (x >= (numOptions - 2))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Missing segment number identifier.\n");
				exitCode = 1;
				break;
			}

			/* Next option encountered must be forced segment number */
			optionString = options[++x];
			if ((localSegmentNumber = GetOptionValue(optionString)) > 0)
				segmentNumber = (unsigned int) localSegmentNumber;
			else
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid segment number requested <%s>.\n",
					   optionString);
				exitCode = 1;
				break;
			}
		}
		/* The last option MUST be the file name */
		else if (x == (numOptions - 1))
		{
			/* Check to see if this looks like an option string before opening */
			if (optionString[0] != '-')
			{
				fp = fopen(optionString, "rb");
				if (fp)
				{
					fileName = options[x];
					if (!(segmentOptions & SEGMENT_NUMBER_FORCED))
						segmentNumber = GetSegmentNumberFromFileName(fileName);
				}
				else
				{
					rc = OPT_RC_FILE;
					printf("Error: Could not open file <%s>.\n", optionString);
					exitCode = 1;
					break;
				}
			}
			else
			{
				/* Could be the case where the help flag is used without a
				 * filename. Otherwise, the last option isn't a file */
				if (strcmp(optionString, "-h") == 0)
					rc = OPT_RC_COPYRIGHT;
				else
				{
					rc = OPT_RC_FILE;
					printf("Error: Missing file name to dump.\n");
					exitCode = 1;
				}
				break;
			}
		}
		else
		{
			unsigned int y;

			/* Option strings must start with '-' and contain switches */
			if (optionString[0] != '-')
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid option string <%s>.\n", optionString);
				exitCode = 1;
				break;
			}

			/* Iterate through the singular option string, throw out
			 * garbage, duplicates and set flags to be used in formatting */
			for (y = 1; y < optionStringLength; y++)
			{
				switch (optionString[y])
				{
						/* Use absolute addressing */
					case 'a':
						SET_OPTION(blockOptions, BLOCK_ABSOLUTE, 'a');
						break;

						/* Dump the binary contents of the page */
					case 'b':
						SET_OPTION(blockOptions, BLOCK_BINARY, 'b');
						break;

						/* Dump the listed file as a control file */
					case 'c':
						SET_OPTION(controlOptions, CONTROL_DUMP, 'c');
						break;

						/* Do not interpret the data. Format to hex and ascii. */
					case 'd':
						SET_OPTION(blockOptions, BLOCK_NO_INTR, 'd');
						break;

						/*
						 * Format the contents of the block with
						 * interpretation of the headers */
					case 'f':
						SET_OPTION(blockOptions, BLOCK_FORMAT, 'f');
						break;

						/* Display the usage screen */
					case 'h':
						rc = OPT_RC_COPYRIGHT;
						break;

						/* Format the items in detail */
					case 'i':
						SET_OPTION(itemOptions, ITEM_DETAIL, 'i');
						break;

						/* Verify block checksums */
					case 'k':
						SET_OPTION(blockOptions, BLOCK_CHECKSUMS, 'k');
						break;

						/* Treat file as pg_filenode.map file */
					case 'm':
						isRelMapFile = true;
						break;

						/* Display old values. Ignore Xmax */
					case 'o':
						SET_OPTION(blockOptions, BLOCK_IGNORE_OLD, 'o');
						break;

					case 't':
						SET_OPTION(blockOptions, BLOCK_DECODE_TOAST, 't');
						break;

					case 'v':
						verbose = true;
						break;

						/* Interpret items as standard index values */
					case 'x':
						SET_OPTION(itemOptions, ITEM_INDEX, 'x');
						if (itemOptions & ITEM_HEAP)
						{
							rc = OPT_RC_INVALID;
							printf("Error: Options <y> and <x> are "
								   "mutually exclusive.\n");
							exitCode = 1;
						}
						break;

						/* Interpret items as heap values */
					case 'y':
						SET_OPTION(itemOptions, ITEM_HEAP, 'y');
						if (itemOptions & ITEM_INDEX)
						{
							rc = OPT_RC_INVALID;
							printf("Error: Options <x> and <y> are "
								   "mutually exclusive.\n");
							exitCode = 1;
						}
						break;

					default:
						rc = OPT_RC_INVALID;
						printf("Error: Unknown option <%c>.\n", optionString[y]);
						exitCode = 1;
						break;
				}

				if (rc)
					break;
			}
		}
	}

	if (rc == OPT_RC_DUPLICATE)
	{
		printf("Error: Duplicate option listed <%c>.\n", duplicateSwitch);
		exitCode = 1;
	}

	/* If the user requested a control file dump, a pure binary
	 * block dump or a non-interpreted formatted dump, mask off
	 * all other block level options (with a few exceptions) */
	if (rc == OPT_RC_VALID)
	{
		/* The user has requested a control file dump, only -f and */
		/* -S are valid... turn off all other formatting */
		if (controlOptions & CONTROL_DUMP)
		{
			if ((blockOptions & ~(BLOCK_FORMAT | BLOCK_FORCED))
				|| (itemOptions))
			{
				rc = OPT_RC_INVALID;
				printf("Error: Invalid options used for Control File dump.\n"
					   "       Only options <Sf> may be used with <c>.\n");
				exitCode = 1;
			}
			else
			{
				controlOptions |=
					(blockOptions & (BLOCK_FORMAT | BLOCK_FORCED));
				blockOptions = itemOptions = 0;
			}
		}
		/* The user has requested a binary block dump... only -R and -f
		 * are honoured */
		else if (blockOptions & BLOCK_BINARY)
		{
			blockOptions &= (BLOCK_BINARY | BLOCK_RANGE | BLOCK_FORCED);
			itemOptions = 0;
		}
		/* The user has requested a non-interpreted dump... only -a, -R
		 * and -f are honoured */
		else if (blockOptions & BLOCK_NO_INTR)
		{
			blockOptions &=
				(BLOCK_NO_INTR | BLOCK_ABSOLUTE | BLOCK_RANGE | BLOCK_FORCED);
			itemOptions = 0;
		}
	}

	return (rc);
}

/* Given the index into the parameter list, convert and return the
 * current string to a number if possible */
static int
GetOptionValue(char *optionString)
{
	unsigned int x;
	int			value = -1;
	int			optionStringLength = strlen(optionString);

	/* Verify the next option looks like a number */
	for (x = 0; x < optionStringLength; x++)
		if (!isdigit((int) optionString[x]))
			break;

	/* Convert the string to a number if it looks good */
	if (x == optionStringLength)
		value = atoi(optionString);

	return (value);
}

/* Read the page header off of block 0 to determine the block size
 * used in this file.  Can be overridden using the -S option. The
 * returned value is the block size of block 0 on disk */
unsigned int
GetBlockSize(FILE *fp)
{
	unsigned int localSize = 0;
	int			bytesRead = 0;
	char		localCache[sizeof(PageHeaderData)];

	/* Read the first header off of block 0 to determine the block size */
	bytesRead = fread(&localCache, 1, sizeof(PageHeaderData), fp);
	rewind(fp);

	if (bytesRead == sizeof(PageHeaderData))
		localSize = (unsigned int) PageGetPageSize(localCache);
	else
	{
		printf("Error: Unable to read full page header from block 0.\n"
			   "  ===> Read %u bytes\n", bytesRead);
		exitCode = 1;
	}

	if (localSize == 0)
	{
		printf("Notice: Block size determined from reading block 0 is zero, using default %d instead.\n", BLCKSZ);
		printf("Hint: Use -S <size> to specify the size manually.\n");
		localSize = BLCKSZ;
	}

	return (localSize);
}

/* Determine the contents of the special section on the block and
 * return this enum value */
static unsigned int
GetSpecialSectionType(char *buffer, Page page)
{
	unsigned int rc;
	unsigned int specialOffset;
	unsigned int specialSize;
	unsigned int specialValue;
	PageHeader	pageHeader = (PageHeader) page;

	/* If this is not a partial header, check the validity of the
	 * special section offset and contents */
	if (bytesToFormat > sizeof(PageHeaderData))
	{
		specialOffset = (unsigned int) pageHeader->pd_special;

		/* Check that the special offset can remain on the block or
		 * the partial block */
		if ((specialOffset == 0) ||
			(specialOffset > blockSize) || (specialOffset > bytesToFormat))
			rc = SPEC_SECT_ERROR_BOUNDARY;
		else
		{
			/* we may need to examine last 2 bytes of page to identify index */
			uint16	   *ptype = (uint16 *) (buffer + blockSize - sizeof(uint16));

			specialSize = blockSize - specialOffset;

			/* If there is a special section, use its size to guess its
			 * contents, checking the last 2 bytes of the page in cases
			 * that are ambiguous.  Note we don't attempt to dereference
			 * the pointers without checking bytesToFormat == blockSize. */
			if (specialSize == 0)
				rc = SPEC_SECT_NONE;
			else if (specialSize == MAXALIGN(sizeof(uint32)))
			{
				/* If MAXALIGN is 8, this could be either a sequence or
				 * SP-GiST or GIN. */
				if (bytesToFormat == blockSize)
				{
					specialValue = *((int *) (buffer + specialOffset));
					if (specialValue == SEQUENCE_MAGIC)
						rc = SPEC_SECT_SEQUENCE;
					else if (specialSize == MAXALIGN(sizeof(SpGistPageOpaqueData)) &&
							 *ptype == SPGIST_PAGE_ID)
						rc = SPEC_SECT_INDEX_SPGIST;
					else if (specialSize == MAXALIGN(sizeof(GinPageOpaqueData)))
						rc = SPEC_SECT_INDEX_GIN;
					else
						rc = SPEC_SECT_ERROR_UNKNOWN;
				}
				else
					rc = SPEC_SECT_ERROR_UNKNOWN;
			}
			/* SP-GiST and GIN have same size special section, so check
			 * the page ID bytes first. */
			else if (specialSize == MAXALIGN(sizeof(SpGistPageOpaqueData)) &&
					 bytesToFormat == blockSize &&
					 *ptype == SPGIST_PAGE_ID)
				rc = SPEC_SECT_INDEX_SPGIST;
			else if (specialSize == MAXALIGN(sizeof(GinPageOpaqueData)))
				rc = SPEC_SECT_INDEX_GIN;
			else if (specialSize > 2 && bytesToFormat == blockSize)
			{
				/* As of 8.3, BTree, Hash, and GIST all have the same size
				 * special section, but the last two bytes of the section
				 * can be checked to determine what's what. */
				if (*ptype <= MAX_BT_CYCLE_ID &&
					specialSize == MAXALIGN(sizeof(BTPageOpaqueData)))
					rc = SPEC_SECT_INDEX_BTREE;
				else if (*ptype == HASHO_PAGE_ID &&
						 specialSize == MAXALIGN(sizeof(HashPageOpaqueData)))
					rc = SPEC_SECT_INDEX_HASH;
				else if (*ptype == GIST_PAGE_ID &&
						 specialSize == MAXALIGN(sizeof(GISTPageOpaqueData)))
					rc = SPEC_SECT_INDEX_GIST;
				else
					rc = SPEC_SECT_ERROR_UNKNOWN;
			}
			else
				rc = SPEC_SECT_ERROR_UNKNOWN;
		}
	}
	else
		rc = SPEC_SECT_ERROR_UNKNOWN;

	return (rc);
}

/*	Check whether page is a btree meta page */
static bool
IsBtreeMetaPage(Page page)
{
	PageHeader	pageHeader = (PageHeader) page;

	if ((PageGetSpecialSize(page) == (MAXALIGN(sizeof(BTPageOpaqueData))))
		&& (bytesToFormat == blockSize))
	{
		BTPageOpaque btpo =
		(BTPageOpaque) ((char *) page + pageHeader->pd_special);

		/* Must check the cycleid to be sure it's really btree. */
		if ((btpo->btpo_cycleid <= MAX_BT_CYCLE_ID) &&
			(btpo->btpo_flags & BTP_META))
			return true;
	}
	return false;
}

/*	Check whether page is a gin meta page */
static bool
IsGinMetaPage(Page page)
{
	if ((PageGetSpecialSize(page) == (MAXALIGN(sizeof(GinPageOpaqueData))))
		&& (bytesToFormat == blockSize))
	{
		GinPageOpaque gpo = GinPageGetOpaque(page);

		if (gpo->flags & GIN_META)
			return true;
	}

	return false;
}

/*	Check whether page is a gin leaf page */
static bool
IsGinLeafPage(Page page)
{
	if ((PageGetSpecialSize(page) == (MAXALIGN(sizeof(GinPageOpaqueData))))
		&& (bytesToFormat == blockSize))
	{
		GinPageOpaque gpo = GinPageGetOpaque(page);

		if (gpo->flags & GIN_LEAF)
			return true;
	}

	return false;
}

/* Check whether page is a SpGist meta page */
static bool
IsSpGistMetaPage(Page page)
{
	if ((PageGetSpecialSize(page) == (MAXALIGN(sizeof(SpGistPageOpaqueData))))
		&& (bytesToFormat == blockSize))
	{
		SpGistPageOpaque spgpo = SpGistPageGetOpaque(page);

		if ((spgpo->spgist_page_id == SPGIST_PAGE_ID) &&
			(spgpo->flags & SPGIST_META))
			return true;
	}

	return false;
}

/* Display a header for the dump so we know the file name, the options
 * used and the time the dump was taken */
static void
CreateDumpFileHeader(int numOptions, char **options)
{
	unsigned int x;
	char		optionBuffer[52] = "\0";

	/* Iterate through the options and cache them.
	 * The maximum we can display is 50 option characters + spaces. */
	for (x = 1; x < (numOptions - 1); x++)
	{
		if ((strlen(optionBuffer) + strlen(options[x])) > 50)
			break;
		strcat(optionBuffer, options[x]);
		if (x < numOptions - 2)
			strcat(optionBuffer, " ");
	}

	printf
		("\n*******************************************************************\n"
		 "* PostgreSQL File/Block Formatted Dump Utility\n"
		 "*\n"
		 "* File: %s\n"
		 "* Options used: %s\n"
		 "*******************************************************************\n",
		 fileName, (strlen(optionBuffer)) ? optionBuffer : "None");
}

/*	Dump out a formatted block header for the requested block */
static int
FormatHeader(char *buffer, Page page, BlockNumber blkno, bool isToast)
{
	int			rc = 0;
	unsigned int headerBytes;
	PageHeader	pageHeader = (PageHeader) page;
	char	   *indent = isToast ? "\t" : "";

	if (!isToast || verbose)
		printf("%s<Header> -----\n", indent);

	/* Only attempt to format the header if the entire header (minus the item
	 * array) is available */
	if (bytesToFormat < offsetof(PageHeaderData, pd_linp[0]))
	{
		headerBytes = bytesToFormat;
		rc = EOF_ENCOUNTERED;
	}
	else
	{
		XLogRecPtr	pageLSN = PageGetLSN(page);
		int			maxOffset = PageGetMaxOffsetNumber(page);
		char		flagString[100];

		headerBytes = offsetof(PageHeaderData, pd_linp[0]);
		blockVersion = (unsigned int) PageGetPageLayoutVersion(page);

		/* The full header exists but we have to check that the item array
		 * is available or how far we can index into it */
		if (maxOffset > 0)
		{
			unsigned int itemsLength = maxOffset * sizeof(ItemIdData);

			if (bytesToFormat < (headerBytes + itemsLength))
			{
				headerBytes = bytesToFormat;
				rc = EOF_ENCOUNTERED;
			}
			else
				headerBytes += itemsLength;
		}

		flagString[0] = '\0';
		if (pageHeader->pd_flags & PD_HAS_FREE_LINES)
			strcat(flagString, "HAS_FREE_LINES|");
		if (pageHeader->pd_flags & PD_PAGE_FULL)
			strcat(flagString, "PAGE_FULL|");
		if (pageHeader->pd_flags & PD_ALL_VISIBLE)
			strcat(flagString, "ALL_VISIBLE|");
		if (strlen(flagString))
			flagString[strlen(flagString) - 1] = '\0';

		/* Interpret the content of the header */
		if (!isToast || verbose)
		{
			printf("%s Block Offset: 0x%08x         Offsets: Lower    %4u (0x%04hx)\n",
					indent, pageOffset, pageHeader->pd_lower, pageHeader->pd_lower);
			printf("%s Block: Size %4d  Version %4u            Upper    %4u (0x%04hx)\n",
					indent, (int) PageGetPageSize(page), blockVersion,
					pageHeader->pd_upper, pageHeader->pd_upper);
			printf("%s LSN:  logid %6d recoff 0x%08x      Special  %4u (0x%04hx)\n",
					indent, (uint32) (pageLSN >> 32), (uint32) pageLSN,
					pageHeader->pd_special, pageHeader->pd_special);
			printf("%s Items: %4d                      Free Space: %4u\n",
					indent, maxOffset, pageHeader->pd_upper - pageHeader->pd_lower);
			printf("%s Checksum: 0x%04x  Prune XID: 0x%08x  Flags: 0x%04x (%s)\n",
					indent, pageHeader->pd_checksum, pageHeader->pd_prune_xid,
					pageHeader->pd_flags, flagString);
			printf("%s Length (including item array): %u\n\n",
					indent, headerBytes);
		}

		/* If it's a btree meta page, print the contents of the meta block. */
		if (IsBtreeMetaPage(page))
		{
			BTMetaPageData *btpMeta = BTPageGetMeta(buffer);

			if (!isToast || verbose)
			{
				printf("%s BTree Meta Data:  Magic (0x%08x)   Version (%u)\n",
						indent, btpMeta->btm_magic, btpMeta->btm_version);
				printf("%s                   Root:     Block (%u)  Level (%u)\n",
						indent, btpMeta->btm_root, btpMeta->btm_level);
				printf("%s                   FastRoot: Block (%u)  Level (%u)\n\n",
					indent, btpMeta->btm_fastroot, btpMeta->btm_fastlevel);
			}
			headerBytes += sizeof(BTMetaPageData);
		}

		/* Eye the contents of the header and alert the user to possible 
		 * problems. */
		if ((maxOffset < 0) ||
			(maxOffset > blockSize) ||
			(blockVersion != PG_PAGE_LAYOUT_VERSION) || /* only one we support */
			(pageHeader->pd_upper > blockSize) ||
			(pageHeader->pd_upper > pageHeader->pd_special) ||
			(pageHeader->pd_lower <
			 (sizeof(PageHeaderData) - sizeof(ItemIdData)))
			|| (pageHeader->pd_lower > blockSize)
			|| (pageHeader->pd_upper < pageHeader->pd_lower)
			|| (pageHeader->pd_special > blockSize))
		{
			printf(" Error: Invalid header information.\n\n");
			exitCode = 1;
		}

		if (blockOptions & BLOCK_CHECKSUMS)
		{
			uint32		delta = (segmentSize / blockSize) * segmentNumber;
			uint16		calc_checksum = pg_checksum_page(page, delta + blkno);

			if (calc_checksum != pageHeader->pd_checksum)
			{
				printf(" Error: checksum failure: calculated 0x%04x.\n\n",
					   calc_checksum);
				exitCode = 1;
			}
		}
	}

	/* If we have reached the end of file while interpreting the header, let
	 * the user know about it */
	if (rc == EOF_ENCOUNTERED)
	{
		if (!isToast || verbose)
		{
			printf("%s Error: End of block encountered within the header."
					" Bytes read: %4u.\n\n", indent, bytesToFormat);
		}
		exitCode = 1;
	}

	/* A request to dump the formatted binary of the block (header,
	 * items and special section).  It's best to dump even on an error
	 * so the user can see the raw image. */
	if (blockOptions & BLOCK_FORMAT)
		FormatBinary(buffer, headerBytes, 0);

	return (rc);
}

/* Copied from ginpostinglist.c */
#define MaxHeapTuplesPerPageBits	11
static uint64
itemptr_to_uint64(const ItemPointer iptr)
{
	uint64		val;

	val = GinItemPointerGetBlockNumber(iptr);
	val <<= MaxHeapTuplesPerPageBits;
	val |= GinItemPointerGetOffsetNumber(iptr);

	return val;
}

static void
uint64_to_itemptr(uint64 val, ItemPointer iptr)
{
	GinItemPointerSetOffsetNumber(iptr, val & ((1 << MaxHeapTuplesPerPageBits) - 1));
	val = val >> MaxHeapTuplesPerPageBits;
	GinItemPointerSetBlockNumber(iptr, val);
}

/*
 * Decode varbyte-encoded integer at *ptr. *ptr is incremented to next integer.
 */
static uint64
decode_varbyte(unsigned char **ptr)
{
	uint64		val;
	unsigned char *p = *ptr;
	uint64		c;

	/* 1st byte */
	c = *(p++);
	val = c & 0x7F;
	if (c & 0x80)
	{
		/* 2nd byte */
		c = *(p++);
		val |= (c & 0x7F) << 7;
		if (c & 0x80)
		{
			/* 3rd byte */
			c = *(p++);
			val |= (c & 0x7F) << 14;
			if (c & 0x80)
			{
				/* 4th byte */
				c = *(p++);
				val |= (c & 0x7F) << 21;
				if (c & 0x80)
				{
					/* 5th byte */
					c = *(p++);
					val |= (c & 0x7F) << 28;
					if (c & 0x80)
					{
						/* 6th byte */
						c = *(p++);
						val |= (c & 0x7F) << 35;
						if (c & 0x80)
						{
							/* 7th byte, should not have continuation bit */
							c = *(p++);
							val |= c << 42;
							Assert((c & 0x80) == 0);
						}
					}
				}
			}
		}
	}

	*ptr = p;

	return val;
}

/*	Dump out gin-specific content of block */
static void
FormatGinBlock(char *buffer,
		bool isToast,
		Oid toastOid,
		unsigned int toastExternalSize,
		char *toastValue,
		unsigned int *toastRead)
{
	Page		page = (Page) buffer;
	char	   *indent = isToast ? "\t" : "";

	if (isToast && !verbose)
		return;

	printf("%s<Data> -----\n", indent);

	if (IsGinLeafPage(page))
	{
		if (GinPageIsCompressed(page))
		{
			GinPostingList *seg = GinDataLeafPageGetPostingList(page);
			int				plist_idx = 1;
			Size			len = GinDataLeafPageGetPostingListSize(page);
			Pointer			endptr = ((Pointer) seg) + len;
			ItemPointer		cur;

			while ((Pointer) seg < endptr)
			{
				int				item_idx = 1;
				uint64			val;
				unsigned char  *endseg = seg->bytes + seg->nbytes;
				unsigned char  *ptr = seg->bytes;

				cur = &seg->first;
				printf("\n%s Posting List	%3d -- Length: %4u\n",
					   indent, plist_idx, seg->nbytes);
				printf("%s	ItemPointer %3d -- Block Id: %4u linp Index: %4u\n",
					   indent, item_idx,
					   ((uint32) ((cur->ip_blkid.bi_hi << 16) |
								  (uint16) cur->ip_blkid.bi_lo)),
					   cur->ip_posid);

				val = itemptr_to_uint64(&seg->first);
				while (ptr < endseg)
				{
					val += decode_varbyte(&ptr);
					item_idx++;

					uint64_to_itemptr(val, cur);
					printf("%s	ItemPointer %3d -- Block Id: %4u linp Index: %4u\n",
						   indent, item_idx,
						   ((uint32) ((cur->ip_blkid.bi_hi << 16) |
									  (uint16) cur->ip_blkid.bi_lo)),
						   cur->ip_posid);
				}

				plist_idx++;

				seg = GinNextPostingListSegment(seg);
			}

		}
		else
		{
			int			i,
						nitems = GinPageGetOpaque(page)->maxoff;
			ItemPointer	items = (ItemPointer) GinDataPageGetData(page);

			for (i = 0; i < nitems; i++)
			{
				printf("%s ItemPointer %d -- Block Id: %u linp Index: %u\n",
					   indent, i + 1,
					   ((uint32) ((items[i].ip_blkid.bi_hi << 16) |
								  (uint16) items[i].ip_blkid.bi_lo)),
					   items[i].ip_posid);
			}
		}
	}
	else
	{
		OffsetNumber	cur,
						high = GinPageGetOpaque(page)->maxoff;
		PostingItem	   *pitem = NULL;

		for (cur = FirstOffsetNumber; cur <= high; cur = OffsetNumberNext(cur))
		{
			pitem = GinDataPageGetPostingItem(page, cur);
			printf("%s PostingItem %d -- child Block Id: (%u) Block Id: %u linp Index: %u\n",
				   indent, cur,
				   ((uint32) ((pitem->child_blkno.bi_hi << 16) |
							  (uint16) pitem->child_blkno.bi_lo)),
				   ((uint32) ((pitem->key.ip_blkid.bi_hi << 16) |
							  (uint16) pitem->key.ip_blkid.bi_lo)),
				   pitem->key.ip_posid);
		}
	}

	printf("\n");
}

/*	Dump out formatted items that reside on this block */
static void
FormatItemBlock(char *buffer,
		Page page,
		bool isToast,
		Oid toastOid,
		unsigned int toastExternalSize,
		char *toastValue,
		unsigned int *toastRead)
{
	unsigned int x;
	unsigned int itemSize;
	unsigned int itemOffset;
	unsigned int itemFlags;
	ItemId		itemId;
	int			maxOffset = PageGetMaxOffsetNumber(page);
	char	   *indent = isToast ? "\t" : "";

	/* If it's a btree meta page, the meta block is where items would normally
	 * be; don't print garbage. */
	if (IsBtreeMetaPage(page))
		return;

	/* Same as above */
	if (IsSpGistMetaPage(page))
		return;

	/* Same as above */
	if (IsGinMetaPage(page))
		return;

	/* Leaf pages of GIN index contain posting lists
	 * instead of item array.
	 */
	if (specialType == SPEC_SECT_INDEX_GIN)
	{
		FormatGinBlock(buffer, isToast, toastOid,
					   toastExternalSize, toastValue,
					   toastRead);
		return;
	}

	if (!isToast || verbose)
		printf("%s<Data> -----\n", indent);

	/* Loop through the items on the block.  Check if the block is
	 * empty and has a sensible item array listed before running
	 * through each item */
	if (maxOffset == 0)
	{
		if (!isToast || verbose)
			printf("%s Empty block - no items listed \n\n", indent);
	}
	else if ((maxOffset < 0) || (maxOffset > blockSize))
	{
		if (!isToast || verbose)
			printf("%s Error: Item index corrupt on block. Offset: <%d>.\n\n",
				   indent,
				   maxOffset);
		exitCode = 1;
	}
	else
	{
		int				formatAs;
		char			textFlags[16];
		uint32			chunkId;
		unsigned int	chunkSize = 0;

		/* First, honour requests to format items a special way, then
		 * use the special section to determine the format style */
		if (itemOptions & ITEM_INDEX)
			formatAs = ITEM_INDEX;
		else if (itemOptions & ITEM_HEAP)
			formatAs = ITEM_HEAP;
		else
			switch (specialType)
			{
				case SPEC_SECT_INDEX_BTREE:
				case SPEC_SECT_INDEX_HASH:
				case SPEC_SECT_INDEX_GIST:
				case SPEC_SECT_INDEX_GIN:
					formatAs = ITEM_INDEX;
					break;
				case SPEC_SECT_INDEX_SPGIST:
					{
						SpGistPageOpaque spgpo =
						(SpGistPageOpaque) ((char *) page +
											((PageHeader) page)->pd_special);

						if (spgpo->flags & SPGIST_LEAF)
							formatAs = ITEM_SPG_LEAF;
						else
							formatAs = ITEM_SPG_INNER;
					}
					break;
				default:
					formatAs = ITEM_HEAP;
					break;
			}

		for (x = 1; x < (maxOffset + 1); x++)
		{
			itemId = PageGetItemId(page, x);
			itemFlags = (unsigned int) ItemIdGetFlags(itemId);
			itemSize = (unsigned int) ItemIdGetLength(itemId);
			itemOffset = (unsigned int) ItemIdGetOffset(itemId);

			switch (itemFlags)
			{
				case LP_UNUSED:
					strcpy(textFlags, "UNUSED");
					break;
				case LP_NORMAL:
					strcpy(textFlags, "NORMAL");
					break;
				case LP_REDIRECT:
					strcpy(textFlags, "REDIRECT");
					break;
				case LP_DEAD:
					strcpy(textFlags, "DEAD");
					break;
				default:
					/* shouldn't be possible */
					sprintf(textFlags, "0x%02x", itemFlags);
					break;
			}

			if (!isToast || verbose)
				printf("%s Item %3u -- Length: %4u  Offset: %4u (0x%04x)"
					   "  Flags: %s\n",
					   indent,
					   x,
					   itemSize,
					   itemOffset,
					   itemOffset,
					   textFlags);

			/* Make sure the item can physically fit on this block before
			 * formatting */
			if ((itemOffset + itemSize > blockSize) ||
				(itemOffset + itemSize > bytesToFormat))
			{
				if (!isToast || verbose)
					printf("%s  Error: Item contents extend beyond block.\n"
						   "%s         BlockSize<%d> Bytes Read<%d> Item Start<%d>.\n",
						   indent, indent, blockSize, bytesToFormat, itemOffset + itemSize);
				exitCode = 1;
			}
			else
			{
				HeapTupleHeader tuple_header;
				TransactionId xmax;

				/* If the user requests that the items be interpreted as
				 * heap or index items... */
				if (itemOptions & ITEM_DETAIL)
					FormatItem(buffer, itemSize, itemOffset, formatAs);

				/* Dump the items contents in hex and ascii */
				if (blockOptions & BLOCK_FORMAT)
					FormatBinary(buffer, itemSize, itemOffset);

				/* Check if tuple was deleted */
				tuple_header = (HeapTupleHeader) (&buffer[itemOffset]);
				xmax = HeapTupleHeaderGetRawXmax(tuple_header);
				if ((blockOptions & BLOCK_IGNORE_OLD) && (xmax != 0))
				{
					if (!isToast || verbose)
						printf("%stuple was removed by transaction #%d\n",
								indent,
								xmax);
				}
				else if (isToast)
				{
					ToastChunkDecode(&buffer[itemOffset], itemSize, toastOid,
									 &chunkId, toastValue + *toastRead,
									 &chunkSize);

					if (!isToast || verbose)
						printf("%s  Read TOAST chunk. TOAST Oid: %d, chunk id: %d, "
							   "chunk data size: %d\n",
							   indent, toastOid, chunkId, chunkSize);

					*toastRead += chunkSize;

					if (*toastRead >= toastExternalSize)
						break;
				}
				else if ((blockOptions & BLOCK_DECODE) && (itemFlags == LP_NORMAL))
				{
					/* Decode tuple data */
					FormatDecode(&buffer[itemOffset], itemSize);
				}

				if (!isToast && x == maxOffset)
					printf("\n");
			}
		}
	}
}

/* Interpret the contents of the item based on whether it has a special
 * section and/or the user has hinted */
static void
FormatItem(char *buffer, unsigned int numBytes, unsigned int startIndex,
		   unsigned int formatAs)
{
	static const char *const spgist_tupstates[4] = {
		"LIVE",
		"REDIRECT",
		"DEAD",
		"PLACEHOLDER"
	};

	if (formatAs == ITEM_INDEX)
	{
		/* It is an IndexTuple item, so dump the index header */
		if (numBytes < sizeof(ItemPointerData))
		{
			if (numBytes)
			{
				printf("  Error: This item does not look like an index item.\n");
				exitCode = 1;
			}
		}
		else
		{
			IndexTuple	itup = (IndexTuple) (&(buffer[startIndex]));

			printf("  Block Id: %u  linp Index: %u  Size: %d\n"
				   "  Has Nulls: %u  Has Varwidths: %u\n\n",
				   ((uint32) ((itup->t_tid.ip_blkid.bi_hi << 16) |
							  (uint16) itup->t_tid.ip_blkid.bi_lo)),
				   itup->t_tid.ip_posid,
				   (int) IndexTupleSize(itup),
				   IndexTupleHasNulls(itup) ? 1 : 0,
				   IndexTupleHasVarwidths(itup) ? 1 : 0);

			if (numBytes != IndexTupleSize(itup))
			{
				printf("  Error: Item size difference. Given <%u>, "
					   "Internal <%d>.\n", numBytes, (int) IndexTupleSize(itup));
				exitCode = 1;
			}
		}
	}
	else if (formatAs == ITEM_SPG_INNER)
	{
		/* It is an SpGistInnerTuple item, so dump the index header */
		if (numBytes < SGITHDRSZ)
		{
			if (numBytes)
			{
				printf("  Error: This item does not look like an SPGiST item.\n");
				exitCode = 1;
			}
		}
		else
		{
			SpGistInnerTuple itup = (SpGistInnerTuple) (&(buffer[startIndex]));

			printf("  State: %s  allTheSame: %d nNodes: %u prefixSize: %u\n\n",
				   spgist_tupstates[itup->tupstate],
				   itup->allTheSame,
				   itup->nNodes,
				   itup->prefixSize);

			if (numBytes != itup->size)
			{
				printf("  Error: Item size difference. Given <%u>, "
					   "Internal <%d>.\n", numBytes, (int) itup->size);
				exitCode = 1;
			}
			else if (itup->prefixSize == MAXALIGN(itup->prefixSize))
			{
				int			i;
				SpGistNodeTuple node;

				/* Dump the prefix contents in hex and ascii */
				if ((blockOptions & BLOCK_FORMAT) &&
					SGITHDRSZ + itup->prefixSize <= numBytes)
					FormatBinary(buffer,
							SGITHDRSZ + itup->prefixSize, startIndex);

				/* Try to print the nodes, but only while pointer is sane */
				SGITITERATE(itup, i, node)
				{
					int			off = (char *) node - (char *) itup;

					if (off + SGNTHDRSZ > numBytes)
						break;
					printf("  Node %2u:  Downlink: %u/%u  Size: %d  Null: %u\n",
						   i,
						   ((uint32) ((node->t_tid.ip_blkid.bi_hi << 16) |
									  (uint16) node->t_tid.ip_blkid.bi_lo)),
						   node->t_tid.ip_posid,
						   (int) IndexTupleSize(node),
						   IndexTupleHasNulls(node) ? 1 : 0);
					/* Dump the node's contents in hex and ascii */
					if ((blockOptions & BLOCK_FORMAT) &&
						off + IndexTupleSize(node) <= numBytes)
						FormatBinary(buffer,
								IndexTupleSize(node), startIndex + off);
					if (IndexTupleSize(node) != MAXALIGN(IndexTupleSize(node)))
						break;
				}
			}
			printf("\n");
		}
	}
	else if (formatAs == ITEM_SPG_LEAF)
	{
		/* It is an SpGistLeafTuple item, so dump the index header */
#if PG_VERSION_NUM >= 140000
		if (numBytes < SGLTHDRSZ(SGLT_GET_HASNULLMASK((SpGistLeafTuple) &(buffer[startIndex]))))
#else
		if (numBytes < SGLTHDRSZ)
#endif
		{
			if (numBytes)
			{
				printf("  Error: This item does not look like an SPGiST item.\n");
				exitCode = 1;
			}
		}
		else
		{
			SpGistLeafTuple itup = (SpGistLeafTuple) (&(buffer[startIndex]));

			printf("  State: %s  nextOffset: %u  Block Id: %u  linp Index: %u\n\n",
				   spgist_tupstates[itup->tupstate],
#if PG_VERSION_NUM >= 140000
				   SGLT_GET_NEXTOFFSET(itup),
#else
				   itup->nextOffset,
#endif
				   ((uint32) ((itup->heapPtr.ip_blkid.bi_hi << 16) |
							  (uint16) itup->heapPtr.ip_blkid.bi_lo)),
				   itup->heapPtr.ip_posid);

			if (numBytes != itup->size)
			{
				printf("  Error: Item size difference. Given <%u>, "
					   "Internal <%d>.\n", numBytes, (int) itup->size);
				exitCode = 1;
			}
		}
	}
	else
	{
		/* It is a HeapTuple item, so dump the heap header */
		int			alignedSize = MAXALIGN(sizeof(HeapTupleHeaderData));

		if (numBytes < alignedSize)
		{
			if (numBytes)
			{
				printf("  Error: This item does not look like a heap item.\n");
				exitCode = 1;
			}
		}
		else
		{
			char		flagString[256];
			unsigned int x;
			unsigned int bitmapLength = 0;
			unsigned int oidLength = 0;
			unsigned int computedLength;
			unsigned int infoMask;
			unsigned int infoMask2;
			int			localNatts;
			unsigned int localHoff;
			bits8	   *localBits;
			unsigned int localBitOffset;

			HeapTupleHeader htup = (HeapTupleHeader) (&buffer[startIndex]);

			infoMask = htup->t_infomask;
			infoMask2 = htup->t_infomask2;
			localBits = &(htup->t_bits[0]);
			localNatts = HeapTupleHeaderGetNatts(htup);
			localHoff = htup->t_hoff;
			localBitOffset = offsetof(HeapTupleHeaderData, t_bits);

			printf("  XMIN: %u  XMAX: %u  CID|XVAC: %u",
				   HeapTupleHeaderGetXmin(htup),
				   HeapTupleHeaderGetRawXmax(htup),
				   HeapTupleHeaderGetRawCommandId(htup));

#if PG_VERSION_NUM < 120000
			if (infoMask & HEAP_HASOID)
				printf("  OID: %u",
					   HeapTupleHeaderGetOid(htup));
#endif

			printf("\n"
				   "  Block Id: %u  linp Index: %u   Attributes: %d   Size: %d\n",
				   ((uint32)
					((htup->t_ctid.ip_blkid.bi_hi << 16) | (uint16) htup->
					 t_ctid.ip_blkid.bi_lo)), htup->t_ctid.ip_posid,
				   localNatts, htup->t_hoff);

			/* Place readable versions of the tuple info mask into a buffer.
			 * Assume that the string can not expand beyond 256. */
			flagString[0] = '\0';
			if (infoMask & HEAP_HASNULL)
				strcat(flagString, "HASNULL|");
			if (infoMask & HEAP_HASVARWIDTH)
				strcat(flagString, "HASVARWIDTH|");
			if (infoMask & HEAP_HASEXTERNAL)
				strcat(flagString, "HASEXTERNAL|");
#if PG_VERSION_NUM < 120000
			if (infoMask & HEAP_HASOID)
				strcat(flagString, "HASOID|");
#endif
			if (infoMask & HEAP_XMAX_KEYSHR_LOCK)
				strcat(flagString, "XMAX_KEYSHR_LOCK|");
			if (infoMask & HEAP_COMBOCID)
				strcat(flagString, "COMBOCID|");
			if (infoMask & HEAP_XMAX_EXCL_LOCK)
				strcat(flagString, "XMAX_EXCL_LOCK|");
			if (infoMask & HEAP_XMAX_LOCK_ONLY)
				strcat(flagString, "XMAX_LOCK_ONLY|");
			if (infoMask & HEAP_XMIN_COMMITTED)
				strcat(flagString, "XMIN_COMMITTED|");
			if (infoMask & HEAP_XMIN_INVALID)
				strcat(flagString, "XMIN_INVALID|");
			if (infoMask & HEAP_XMAX_COMMITTED)
				strcat(flagString, "XMAX_COMMITTED|");
			if (infoMask & HEAP_XMAX_INVALID)
				strcat(flagString, "XMAX_INVALID|");
			if (infoMask & HEAP_XMAX_IS_MULTI)
				strcat(flagString, "XMAX_IS_MULTI|");
			if (infoMask & HEAP_UPDATED)
				strcat(flagString, "UPDATED|");
			if (infoMask & HEAP_MOVED_OFF)
				strcat(flagString, "MOVED_OFF|");
			if (infoMask & HEAP_MOVED_IN)
				strcat(flagString, "MOVED_IN|");

			if (infoMask2 & HEAP_KEYS_UPDATED)
				strcat(flagString, "KEYS_UPDATED|");
			if (infoMask2 & HEAP_HOT_UPDATED)
				strcat(flagString, "HOT_UPDATED|");
			if (infoMask2 & HEAP_ONLY_TUPLE)
				strcat(flagString, "HEAP_ONLY|");

			if (strlen(flagString))
				flagString[strlen(flagString) - 1] = '\0';

			printf("  infomask: 0x%04x (%s) \n", infoMask, flagString);

			/* As t_bits is a variable length array, determine the length of
			 * the header proper */
			if (infoMask & HEAP_HASNULL)
				bitmapLength = BITMAPLEN(localNatts);
			else
				bitmapLength = 0;

#if PG_VERSION_NUM < 120000
			if (infoMask & HEAP_HASOID)
				oidLength += sizeof(Oid);
#endif

			computedLength =
				MAXALIGN(localBitOffset + bitmapLength + oidLength);

			/* Inform the user of a header size mismatch or dump the t_bits
			 * array */
			if (computedLength != localHoff)
			{
				printf
					("  Error: Computed header length not equal to header size.\n"
					 "         Computed <%u>  Header: <%d>\n", computedLength,
					 localHoff);

				exitCode = 1;
			}
			else if ((infoMask & HEAP_HASNULL) && bitmapLength)
			{
				printf("  t_bits: ");
				for (x = 0; x < bitmapLength; x++)
				{
					printf("[%u]: 0x%02x ", x, localBits[x]);
					if (((x & 0x03) == 0x03) && (x < bitmapLength - 1))
						printf("\n          ");
				}
				printf("\n");
			}
			printf("\n");
		}
	}
}


/* On blocks that have special sections, print the contents
 * according to previously determined special section type */
static void
FormatSpecial(char *buffer)
{
	PageHeader	pageHeader = (PageHeader) buffer;
	char		flagString[100] = "\0";
	unsigned int specialOffset = pageHeader->pd_special;
	unsigned int specialSize =
	(blockSize >= specialOffset) ? (blockSize - specialOffset) : 0;

	printf("<Special Section> -----\n");

	switch (specialType)
	{
		case SPEC_SECT_ERROR_UNKNOWN:
		case SPEC_SECT_ERROR_BOUNDARY:
			printf(" Error: Invalid special section encountered.\n");
			exitCode = 1;
			break;

		case SPEC_SECT_SEQUENCE:
			printf(" Sequence: 0x%08x\n", SEQUENCE_MAGIC);
			break;

			/* Btree index section */
		case SPEC_SECT_INDEX_BTREE:
			{
				BTPageOpaque btreeSection = (BTPageOpaque) (buffer + specialOffset);

				if (btreeSection->btpo_flags & BTP_LEAF)
					strcat(flagString, "LEAF|");
				if (btreeSection->btpo_flags & BTP_ROOT)
					strcat(flagString, "ROOT|");
				if (btreeSection->btpo_flags & BTP_DELETED)
					strcat(flagString, "DELETED|");
				if (btreeSection->btpo_flags & BTP_META)
					strcat(flagString, "META|");
				if (btreeSection->btpo_flags & BTP_HALF_DEAD)
					strcat(flagString, "HALFDEAD|");
				if (btreeSection->btpo_flags & BTP_SPLIT_END)
					strcat(flagString, "SPLITEND|");
				if (btreeSection->btpo_flags & BTP_HAS_GARBAGE)
					strcat(flagString, "HASGARBAGE|");
				if (btreeSection->btpo_flags & BTP_INCOMPLETE_SPLIT)
					strcat(flagString, "INCOMPLETESPLIT|");
#if PG_VERSION_NUM >= 140000
				if (btreeSection->btpo_flags & BTP_HAS_FULLXID)
					strcat(flagString, "HASFULLXID|");
#endif
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';

				printf(" BTree Index Section:\n"
					   "  Flags: 0x%04x (%s)\n"
					   "  Blocks: Previous (%d)  Next (%d)  %s (%d)  CycleId (%d)\n\n",
					   btreeSection->btpo_flags, flagString,
					   btreeSection->btpo_prev, btreeSection->btpo_next,
					   (btreeSection->
						btpo_flags & BTP_DELETED) ? "Next XID" : "Level",
#if PG_VERSION_NUM >= 140000
					   btreeSection->btpo_level,
#else
					   btreeSection->btpo.level,
#endif
					   btreeSection->btpo_cycleid);
			}
			break;

			/* Hash index section */
		case SPEC_SECT_INDEX_HASH:
			{
				HashPageOpaque hashSection = (HashPageOpaque) (buffer + specialOffset);

				if ((hashSection->hasho_flag & LH_PAGE_TYPE) == LH_UNUSED_PAGE)
					strcat(flagString, "UNUSED|");
				if (hashSection->hasho_flag & LH_OVERFLOW_PAGE)
					strcat(flagString, "OVERFLOW|");
				if (hashSection->hasho_flag & LH_BUCKET_PAGE)
					strcat(flagString, "BUCKET|");
				if (hashSection->hasho_flag & LH_BITMAP_PAGE)
					strcat(flagString, "BITMAP|");
				if (hashSection->hasho_flag & LH_META_PAGE)
					strcat(flagString, "META|");
				if (hashSection->hasho_flag & LH_BUCKET_BEING_POPULATED)
					strcat(flagString, "BUCKET_BEING_POPULATED|");
				if (hashSection->hasho_flag & LH_BUCKET_BEING_SPLIT)
					strcat(flagString, "BUCKET_BEING_SPLIT|");
				if (hashSection->hasho_flag & LH_BUCKET_NEEDS_SPLIT_CLEANUP)
					strcat(flagString, "BUCKET_NEEDS_SPLIT_CLEANUP|");
				if (hashSection->hasho_flag & LH_PAGE_HAS_DEAD_TUPLES)
					strcat(flagString, "PAGE_HAS_DEAD_TUPLES|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';
				printf(" Hash Index Section:\n"
					   "  Flags: 0x%04x (%s)\n"
					   "  Bucket Number: 0x%04x\n"
					   "  Blocks: Previous (%d)  Next (%d)\n\n",
					   hashSection->hasho_flag, flagString,
					   hashSection->hasho_bucket,
					   hashSection->hasho_prevblkno, hashSection->hasho_nextblkno);
			}
			break;

			/* GIST index section */
		case SPEC_SECT_INDEX_GIST:
			{
				GISTPageOpaque gistSection = (GISTPageOpaque) (buffer + specialOffset);

				if (gistSection->flags & F_LEAF)
					strcat(flagString, "LEAF|");
				if (gistSection->flags & F_DELETED)
					strcat(flagString, "DELETED|");
				if (gistSection->flags & F_TUPLES_DELETED)
					strcat(flagString, "TUPLES_DELETED|");
				if (gistSection->flags & F_FOLLOW_RIGHT)
					strcat(flagString, "FOLLOW_RIGHT|");
				if (gistSection->flags & F_HAS_GARBAGE)
					strcat(flagString, "HAS_GARBAGE|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';
				printf(" GIST Index Section:\n"
					   "  NSN: 0x%08x/0x%08x\n"
					   "  RightLink: %d\n"
					   "  Flags: 0x%08x (%s)\n\n",
					   gistSection->nsn.xlogid, gistSection->nsn.xrecoff,
					   gistSection->rightlink,
					   gistSection->flags, flagString);
			}
			break;

			/* GIN index section */
		case SPEC_SECT_INDEX_GIN:
			{
				GinPageOpaque ginSection = (GinPageOpaque) (buffer + specialOffset);

				if (ginSection->flags & GIN_DATA)
					strcat(flagString, "DATA|");
				if (ginSection->flags & GIN_LEAF)
					strcat(flagString, "LEAF|");
				if (ginSection->flags & GIN_DELETED)
					strcat(flagString, "DELETED|");
				if (ginSection->flags & GIN_META)
					strcat(flagString, "META|");
				if (ginSection->flags & GIN_LIST)
					strcat(flagString, "LIST|");
				if (ginSection->flags & GIN_LIST_FULLROW)
					strcat(flagString, "FULLROW|");
				if (ginSection->flags & GIN_INCOMPLETE_SPLIT)
					strcat(flagString, "INCOMPLETESPLIT|");
				if (ginSection->flags & GIN_COMPRESSED)
					strcat(flagString, "COMPRESSED|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';
				printf(" GIN Index Section:\n"
					   "  Flags: 0x%08x (%s)  Maxoff: %d\n"
					   "  Blocks: RightLink (%d)\n\n",
					   ginSection->flags, flagString,
					   ginSection->maxoff,
					   ginSection->rightlink);
			}
			break;

			/* SP-GIST index section */
		case SPEC_SECT_INDEX_SPGIST:
			{
				SpGistPageOpaque spgistSection = (SpGistPageOpaque) (buffer + specialOffset);

				if (spgistSection->flags & SPGIST_META)
					strcat(flagString, "META|");
				if (spgistSection->flags & SPGIST_DELETED)
					strcat(flagString, "DELETED|");
				if (spgistSection->flags & SPGIST_LEAF)
					strcat(flagString, "LEAF|");
				if (spgistSection->flags & SPGIST_NULLS)
					strcat(flagString, "NULLS|");
				if (strlen(flagString))
					flagString[strlen(flagString) - 1] = '\0';
				printf(" SPGIST Index Section:\n"
					   "  Flags: 0x%08x (%s)\n"
					   "  nRedirection: %d\n"
					   "  nPlaceholder: %d\n\n",
					   spgistSection->flags, flagString,
					   spgistSection->nRedirection,
					   spgistSection->nPlaceholder);
			}
			break;

			/* No idea what type of special section this is */
		default:
			printf(" Unknown special section type. Type: <%u>.\n", specialType);
			exitCode = 1;
			break;
	}

	/* Dump the formatted contents of the special section */
	if (blockOptions & BLOCK_FORMAT)
	{
		if (specialType == SPEC_SECT_ERROR_BOUNDARY)
		{
			printf(" Error: Special section points off page."
				   " Unable to dump contents.\n");

			exitCode = 1;
		}
		else
			FormatBinary(buffer, specialSize, specialOffset);
	}
}

/*	For each block, dump out formatted header and content information */
static void
FormatBlock(unsigned int blockOptions,
		unsigned int controlOptions,
		char *buffer,
		BlockNumber currentBlock,
		unsigned int blockSize,
		bool isToast,
		Oid toastOid,
		unsigned int toastExternalSize,
		char *toastValue,
		unsigned int *toastRead)
{
	Page		page = (Page) buffer;
	char	   *indent = isToast ? "\t" : "";

	pageOffset = blockSize * currentBlock;
	specialType = GetSpecialSectionType(buffer, page);

	if (!isToast || verbose)
		printf("\n%sBlock %4u **%s***************************************\n",
			   indent,
			   currentBlock,
			   (bytesToFormat ==
				blockSize) ? "***************" : " PARTIAL BLOCK ");

	/* Either dump out the entire block in hex+acsii fashion or
	 * interpret the data based on block structure */
	if (blockOptions & BLOCK_NO_INTR)
		FormatBinary(buffer, bytesToFormat, 0);
	else
	{
		int			rc;

		/* Every block contains a header, items and possibly a special
		 * section.  Beware of partial block reads though */
		rc = FormatHeader(buffer, page, currentBlock, isToast);

		/* If we didn't encounter a partial read in the header, carry on... */
		if (rc != EOF_ENCOUNTERED)
		{
			FormatItemBlock(buffer,
					page,
					isToast,
					toastOid,
					toastExternalSize,
					toastValue,
					toastRead);

			if (specialType != SPEC_SECT_NONE)
				FormatSpecial(buffer);
		}
	}
}

/*	Dump out the content of the PG control file */
static void
FormatControl(char *buffer)
{
	unsigned int localPgVersion = 0;
	unsigned int controlFileSize = 0;
	time_t		cd_time;
	time_t		cp_time;

	printf
		("\n<pg_control Contents> *********************************************\n\n");

	/* Check the version */
	if (bytesToFormat >= offsetof(ControlFileData, catalog_version_no))
		localPgVersion = ((ControlFileData *) buffer)->pg_control_version;

	if (localPgVersion >= 72)
		controlFileSize = sizeof(ControlFileData);
	else
	{
		printf("pg_filedump: pg_control version %u not supported.\n",
			   localPgVersion);
		return;
	}

	/* Interpret the control file if it's all there */
	if (bytesToFormat >= controlFileSize)
	{
		ControlFileData *controlData = (ControlFileData *) buffer;
		CheckPoint *checkPoint = &(controlData->checkPointCopy);
		pg_crc32	crcLocal;
		char	   *dbState;

		/* Compute a local copy of the CRC to verify the one on disk */
		INIT_CRC32C(crcLocal);
		COMP_CRC32C(crcLocal, buffer, offsetof(ControlFileData, crc));
		FIN_CRC32C(crcLocal);

		/* Grab a readable version of the database state */
		switch (controlData->state)
		{
			case DB_STARTUP:
				dbState = "STARTUP";
				break;
			case DB_SHUTDOWNED:
				dbState = "SHUTDOWNED";
				break;
			case DB_SHUTDOWNED_IN_RECOVERY:
				dbState = "SHUTDOWNED_IN_RECOVERY";
				break;
			case DB_SHUTDOWNING:
				dbState = "SHUTDOWNING";
				break;
			case DB_IN_CRASH_RECOVERY:
				dbState = "IN CRASH RECOVERY";
				break;
			case DB_IN_ARCHIVE_RECOVERY:
				dbState = "IN ARCHIVE RECOVERY";
				break;
			case DB_IN_PRODUCTION:
				dbState = "IN PRODUCTION";
				break;
			default:
				dbState = "UNKNOWN";
				break;
		}

		/* convert timestamps to system's time_t width */
		cd_time = controlData->time;
		cp_time = checkPoint->time;

		printf("                          CRC: %s\n"
			   "           pg_control Version: %u%s\n"
			   "              Catalog Version: %u\n"
			   "            System Identifier: " UINT64_FORMAT "\n"
			   "                        State: %s\n"
			   "                Last Mod Time: %s"
			   "       Last Checkpoint Record: Log File (%u) Offset (0x%08x)\n"
#if PG_VERSION_NUM < 110000
			   "   Previous Checkpoint Record: Log File (%u) Offset (0x%08x)\n"
#endif
			   "  Last Checkpoint Record Redo: Log File (%u) Offset (0x%08x)\n"
			   "             |-    TimeLineID: %u\n"
			   "             |-      Next XID: %u/%u\n"
			   "             |-      Next OID: %u\n"
			   "             |-    Next Multi: %u\n"
			   "             |- Next MultiOff: %u\n"
			   "             |-          Time: %s"
			   "       Minimum Recovery Point: Log File (%u) Offset (0x%08x)\n"
			   "       Maximum Data Alignment: %u\n"
			   "        Floating-Point Sample: %.7g%s\n"
			   "          Database Block Size: %u\n"
			   "           Blocks Per Segment: %u\n"
			   "              XLOG Block Size: %u\n"
			   "            XLOG Segment Size: %u\n"
			   "    Maximum Identifier Length: %u\n"
			   "           Maximum Index Keys: %u\n"
			   "             TOAST Chunk Size: %u\n\n",
			   EQ_CRC32C(crcLocal,
						 controlData->crc) ? "Correct" : "Not Correct",
			   controlData->pg_control_version,
			   (controlData->pg_control_version == PG_CONTROL_VERSION ?
				"" : " (Not Correct!)"),
			   controlData->catalog_version_no,
			   controlData->system_identifier,
			   dbState,
			   ctime(&(cd_time)),
			   (uint32) (controlData->checkPoint >> 32), (uint32) controlData->checkPoint,
#if PG_VERSION_NUM < 110000
			   (uint32) (controlData->prevCheckPoint >> 32), (uint32) controlData->prevCheckPoint,
#endif
			   (uint32) (checkPoint->redo >> 32), (uint32) checkPoint->redo,
			   checkPoint->ThisTimeLineID,
#if PG_VERSION_NUM < 120000
			   checkPoint->nextXidEpoch, checkPoint->nextXid,
#elif PG_VERSION_NUM < 140000
			   EpochFromFullTransactionId(checkPoint->nextFullXid),
			   XidFromFullTransactionId(checkPoint->nextFullXid),
#else
			   EpochFromFullTransactionId(checkPoint->nextXid),
			   XidFromFullTransactionId(checkPoint->nextXid),
#endif
			   checkPoint->nextOid,
			   checkPoint->nextMulti, checkPoint->nextMultiOffset,
			   ctime(&cp_time),
			   (uint32) (controlData->minRecoveryPoint >> 32), (uint32) controlData->minRecoveryPoint,
			   controlData->maxAlign,
			   controlData->floatFormat,
			   (controlData->floatFormat == FLOATFORMAT_VALUE ?
				"" : " (Not Correct!)"),
			   controlData->blcksz,
			   controlData->relseg_size,
			   controlData->xlog_blcksz,
			   controlData->xlog_seg_size,
			   controlData->nameDataLen,
			   controlData->indexMaxKeys,
			   controlData->toast_max_chunk_size);
	}
	else
	{
		printf(" Error: pg_control file size incorrect.\n"
			   "        Size: Correct <%u>  Received <%u>.\n\n",
			   controlFileSize, bytesToFormat);

		/* If we have an error, force a formatted dump so we can see
		 * where things are going wrong */
		controlOptions |= CONTROL_FORMAT;

		exitCode = 1;
	}

	/* Dump hex and ascii representation of data */
	if (controlOptions & CONTROL_FORMAT)
	{
		printf("<pg_control Formatted Dump> *****************"
			   "**********************\n\n");
		FormatBinary(buffer, bytesToFormat, 0);
	}
}

/* Dump out the contents of the block in hex and ascii.
 * BYTES_PER_LINE bytes are formatted in each line. */
static void
FormatBinary(char *buffer, unsigned int numBytes, unsigned int startIndex)
{
	unsigned int index = 0;
	unsigned int stopIndex = 0;
	unsigned int x = 0;
	unsigned int lastByte = startIndex + numBytes;

	if (numBytes)
	{
		/* Iterate through a printable row detailing the current
		 * address, the hex and ascii values */
		for (index = startIndex; index < lastByte; index += BYTES_PER_LINE)
		{
			stopIndex = index + BYTES_PER_LINE;

			/* Print out the address */
			if (blockOptions & BLOCK_ABSOLUTE)
				printf("  %08x: ", (unsigned int) (pageOffset + index));
			else
				printf("  %04x: ", (unsigned int) index);

			/* Print out the hex version of the data */
			for (x = index; x < stopIndex; x++)
			{
				if (x < lastByte)
					printf("%02x", 0xff & ((unsigned) buffer[x]));
				else
					printf("  ");
				if ((x & 0x03) == 0x03)
					printf(" ");
			}
			printf(" ");

			/* Print out the ascii version of the data */
			for (x = index; x < stopIndex; x++)
			{
				if (x < lastByte)
					printf("%c", isprint(buffer[x]) ? buffer[x] : '.');
				else
					printf(" ");
			}
			printf("\n");
		}
		printf("\n");
	}
}

/* Dump the binary image of the block */
static void
DumpBinaryBlock(char *buffer)
{
	unsigned int x;

	for (x = 0; x < bytesToFormat; x++)
		putchar(buffer[x]);
}

/* Control the dumping of the blocks within the file */
int
DumpFileContents(unsigned int blockOptions,
		unsigned int controlOptions,
		FILE *fp,
		unsigned int blockSize,
		int blockStart,
		int blockEnd,
		bool isToast,
		Oid toastOid,
		unsigned int toastExternalSize,
		char *toastValue)
{
	unsigned int	initialRead = 1;
	unsigned int	contentsToDump = 1;
	unsigned int	toastDataRead = 0;
	BlockNumber		currentBlock = 0;
	int				result = 0;
	/* On a positive block size, allocate a local buffer to store
	 * the subsequent blocks */
	char		   *block = (char *)malloc(blockSize);
	if (!block)
	{
		printf("\nError: Unable to create buffer of size <%d>.\n",
			   blockSize);
		result = 1;
	}

	/* If the user requested a block range, seek to the correct position
	 * within the file for the start block. */
	if (result == 0 && blockOptions & BLOCK_RANGE)
	{
		unsigned int	position = blockSize * blockStart;

		if (fseek(fp, position, SEEK_SET) != 0)
		{
			printf("Error: Seek error encountered before requested "
				   "start block <%d>.\n", blockStart);
			contentsToDump = 0;
			result = 1;
		}
		else
			currentBlock = blockStart;
	}

	/* Iterate through the blocks in the file until you reach the end or
	 * the requested range end */
	while (contentsToDump && result == 0)
	{
		bytesToFormat = fread(block, 1, blockSize, fp);

		if (bytesToFormat == 0)
		{
			/* fseek() won't pop an error if you seek passed eof. The next
			 * subsequent read gets the error. */
			if (initialRead)
				printf("Error: Premature end of file encountered.\n");
			else if (!(blockOptions & BLOCK_BINARY))
				printf("\n*** End of File Encountered. Last Block "
					   "Read: %d ***\n", currentBlock - 1);

			contentsToDump = 0;
		}
		else
		{
			if (blockOptions & BLOCK_BINARY)
				DumpBinaryBlock(block);
			else
			{
				if (controlOptions & CONTROL_DUMP)
				{
					FormatControl(block);
					contentsToDump = false;
				}
				else
				{
					FormatBlock(blockOptions,
							controlOptions,
							block,
							currentBlock,
							blockSize,
							isToast,
							toastOid,
							toastExternalSize,
							toastValue,
							&toastDataRead);
				}
			}
		}

		/* Check to see if we are at the end of the requested range. */
		if ((blockOptions & BLOCK_RANGE) &&
			(currentBlock >= blockEnd) && (contentsToDump))
		{
			/* Don't print out message if we're doing a binary dump */
			if (!(blockOptions & BLOCK_BINARY))
				printf("\n*** End of Requested Range Encountered. "
					   "Last Block Read: %d ***\n", currentBlock);
			contentsToDump = 0;
		}
		else
			currentBlock++;

		initialRead = 0;

		/* If TOAST data is read */
		if (isToast && toastDataRead >= toastExternalSize)
			break;
	}

	free(block);

	return result;
}

int
PrintRelMappings(void)
{
	// For storing ingested data
	char charbuf[RELMAPPER_FILESIZE];
	RelMapFile *map;
	RelMapping *mappings;
	RelMapping m;
	int bytesRead;

	// For confirming Magic Number correctness
	char m1[RELMAPPER_MAGICSIZE];
	char m2[RELMAPPER_MAGICSIZE];
	int magic_ref = RELMAPPER_FILEMAGIC;
	int magic_val;
	int num_loops;

	// Read in the file
	rewind(fp); // Make sure to start from the beginning
	bytesRead = fread(charbuf,1,RELMAPPER_FILESIZE,fp);
	if ( bytesRead != RELMAPPER_FILESIZE ) {
		printf("Read %d bytes, expected %d\n", bytesRead, RELMAPPER_FILESIZE);
		return 0;
	}

	// Convert to RelMapFile type for usability
	map = (RelMapFile *) charbuf;


	// Check and print Magic Number correctness
	printf("Magic Number: 0x%x",map->magic);
	magic_val = map->magic;

	memcpy(m1,&magic_ref,RELMAPPER_MAGICSIZE);
	memcpy(m2,&magic_val,RELMAPPER_MAGICSIZE);
	if ( memcmp(m1,m2,RELMAPPER_MAGICSIZE) == 0 ) {
		printf(" (CORRECT)\n");
	} else {
		printf(" (INCORRECT)\n");
	}

	// Print Mappings
	printf("Num Mappings: %d\n",map->num_mappings);
	printf("Detailed Mappings list:\n");
	mappings = map->mappings;

	// Limit number of mappings as per MAX_MAPPINGS
	num_loops = map->num_mappings;
	if ( map->num_mappings > MAX_MAPPINGS ) {
		num_loops = MAX_MAPPINGS;
		printf("  NOTE: listing has been limited to the first %d mappings\n", MAX_MAPPINGS);
		printf("        (perhaps your file is not a valid pg_filenode.map file?)\n");
	}

	for (int i=0; i < num_loops; i++) {
		m = mappings[i];
		printf("OID: %u\tFilenode: %u\n",
			m.mapoid,
			m.mapfilenode);
	}
	return 1;
}

/* Consume the options and iterate through the given file, formatting as
 * requested. */
int
main(int argv, char **argc)
{
	/* If there is a parameter list, validate the options */
	unsigned int validOptions;

	validOptions = (argv < 2) ? OPT_RC_COPYRIGHT : ConsumeOptions(argv, argc);

	/* Display valid options if no parameters are received or invalid options
	 * where encountered */
	if (validOptions != OPT_RC_VALID)
		DisplayOptions(validOptions);
	else if (isRelMapFile)
	{
		CreateDumpFileHeader(argv, argc);
		exitCode = PrintRelMappings();
	}
	else
	{
		/* Don't dump the header if we're dumping binary pages */
		if (!(blockOptions & BLOCK_BINARY))
			CreateDumpFileHeader(argv, argc);

		/* If the user has not forced a block size, use the size of the
		 * control file data or the information from the block 0 header */
		if (controlOptions)
		{
			if (!(controlOptions & CONTROL_FORCED))
				blockSize = sizeof(ControlFileData);
		}
		else if (!(blockOptions & BLOCK_FORCED))
			blockSize = GetBlockSize(fp);

		exitCode = DumpFileContents(blockOptions,
				controlOptions,
				fp,
				blockSize,
				blockStart,
				blockEnd,
				false /* is toast realtion */,
				0,    /* no toast Oid */
				0,    /* no toast external size */
				NULL  /* no out toast value */
				);
	}

	if (fp)
		fclose(fp);

	exit(exitCode);
}
