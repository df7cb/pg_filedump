/*
 * pg_filedump.c - PostgreSQL file dump utility for dumping and
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

#include "pg_filedump.h"

#include "utils/pg_crc_tables.h"

// Global variables for ease of use mostly
static FILE *fp = NULL;		// File to dump or format
static char *fileName = NULL;	// File name for display
static char *buffer = NULL;	// Cache for current block
static unsigned int blockSize = 0;	// Current block size
static unsigned int currentBlock = 0;	// Current block in file
static unsigned int pageOffset = 0;	// Offset of current block
static unsigned int bytesToFormat = 0;	// Number of bytes to format
static unsigned int blockVersion = 0;	// Block version number

// Function Prototypes
static void DisplayOptions (unsigned int validOptions);
static unsigned int ConsumeOptions (int numOptions, char **options);
static int GetOptionValue (char *optionString);
static void FormatBlock ();
static unsigned int GetBlockSize ();
static unsigned int GetSpecialSectionType (Page page);
static bool IsBtreeMetaPage(Page page);
static void CreateDumpFileHeader (int numOptions, char **options);
static int FormatHeader (Page page);
static void FormatItemBlock (Page page);
static void FormatItem (unsigned int numBytes, unsigned int startIndex,
			unsigned int formatAs);
static void FormatSpecial ();
static void FormatControl ();
static void FormatBinary (unsigned int numBytes, unsigned int startIndex);
static void DumpBinaryBlock ();
static void DumpFileContents ();


// Send properly formed usage information to the user.
static void
DisplayOptions (unsigned int validOptions)
{
  if (validOptions == OPT_RC_COPYRIGHT)
    printf
      ("\nVersion %s (for %s)"
       "\nCopyright (c) 2002-2010 Red Hat, Inc."
       "\nCopyright (c) 2011-2012, PostgreSQL Global Development Group\n",
       FD_VERSION, FD_PG_VERSION);

  printf
    ("\nUsage: pg_filedump [-abcdfhixy] [-R startblock [endblock]] [-S blocksize] file\n\n"
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
     "  -f  Display formatted block content dump along with interpretation\n"
     "  -h  Display this information\n"
     "  -i  Display interpreted item details\n"
     "  -R  Display specific block ranges within the file (Blocks are\n"
     "      indexed from 0)\n" "        [startblock]: block to start at\n"
     "        [endblock]: block to end at\n"
     "      A startblock without an endblock will format the single block\n"
     "  -S  Force block size to [blocksize]\n"
     "  -x  Force interpreted formatting of block items as index items\n"
     "  -y  Force interpreted formatting of block items as heap items\n\n"
     "The following options are valid for control files:\n"
     "  -c  Interpret the file listed as a control file\n"
     "  -f  Display formatted content dump along with interpretation\n"
     "  -S  Force block size to [blocksize]\n"
     "\nReport bugs to <pgsql-bugs@postgresql.org>\n");
}

// Iterate through the provided options and set the option flags.
// An error will result in a positive rc and will force a display
// of the usage information.  This routine returns enum
// optionReturnCode values.
static unsigned int
ConsumeOptions (int numOptions, char **options)
{
  unsigned int rc = OPT_RC_VALID;
  unsigned int x;
  unsigned int optionStringLength;
  char *optionString;
  char duplicateSwitch = 0x00;

  for (x = 1; x < numOptions; x++)
    {
      optionString = options[x];
      optionStringLength = strlen (optionString);

      // Range is a special case where we have to consume the next 1 or 2
      // parameters to mark the range start and end
      if ((optionStringLength == 2) && (strcmp (optionString, "-R") == 0))
	{
	  int range = 0;

	  SET_OPTION (blockOptions, BLOCK_RANGE, 'R');
	  // Only accept the range option once
	  if (rc == OPT_RC_DUPLICATE)
	    break;

	  // Make sure there are options after the range identifier
	  if (x >= (numOptions - 2))
	    {
	      rc = OPT_RC_INVALID;
	      printf ("Error: Missing range start identifier.\n");
	      break;
	    }

	  // Mark that we have the range and advance the option to what should
	  // be the range start. Check the value of the next parameter
	  optionString = options[++x];
	  if ((range = GetOptionValue (optionString)) < 0)
	    {
	      rc = OPT_RC_INVALID;
	      printf ("Error: Invalid range start identifier <%s>.\n",
		      optionString);
	      break;
	    }

	  // The default is to dump only one block
	  blockStart = blockEnd = (unsigned int) range;

	  // We have our range start marker, check if there is an end
	  // marker on the option line.  Assume that the last option
	  // is the file we are dumping, so check if there are options
	  // range start marker and the file
	  if (x <= (numOptions - 3))
	    {
	      if ((range = GetOptionValue (options[x + 1])) >= 0)
		{
		  // End range must be => start range
		  if (blockStart <= range)
		    {
		      blockEnd = (unsigned int) range;
		      x++;
		    }
		  else
		    {
		      rc = OPT_RC_INVALID;
		      printf ("Error: Requested block range start <%d> is "
			      "greater than end <%d>.\n", blockStart, range);
		      break;
		    }
		}
	    }
	}
      // Check for the special case where the user forces a block size
      // instead of having the tool determine it.  This is useful if
      // the header of block 0 is corrupt and gives a garbage block size
      else if ((optionStringLength == 2)
	       && (strcmp (optionString, "-S") == 0))
	{
	  int localBlockSize;

	  SET_OPTION (blockOptions, BLOCK_FORCED, 'S');
	  // Only accept the forced size option once
	  if (rc == OPT_RC_DUPLICATE)
	    break;

	  // The token immediately following -S is the block size
	  if (x >= (numOptions - 2))
	    {
	      rc = OPT_RC_INVALID;
	      printf ("Error: Missing block size identifier.\n");
	      break;
	    }

	  // Next option encountered must be forced block size
	  optionString = options[++x];
	  if ((localBlockSize = GetOptionValue (optionString)) > 0)
	    blockSize = (unsigned int) localBlockSize;
	  else
	    {
	      rc = OPT_RC_INVALID;
	      printf ("Error: Invalid block size requested <%s>.\n",
		      optionString);
	      break;
	    }
	}
      // The last option MUST be the file name
      else if (x == (numOptions - 1))
	{
	  // Check to see if this looks like an option string before opening
	  if (optionString[0] != '-')
	    {
	      fp = fopen (optionString, "rb");
	      if (fp)
		fileName = options[x];
	      else
		{
		  rc = OPT_RC_FILE;
		  printf ("Error: Could not open file <%s>.\n", optionString);
		  break;
		}
	    }
	  else
	    {
	      // Could be the case where the help flag is used without a
	      // filename. Otherwise, the last option isn't a file
	      if (strcmp (optionString, "-h") == 0)
		rc = OPT_RC_COPYRIGHT;
	      else
		{
		  rc = OPT_RC_FILE;
		  printf ("Error: Missing file name to dump.\n");
		}
	      break;
	    }
	}
      else
	{
	  unsigned int y;

	  // Option strings must start with '-' and contain switches
	  if (optionString[0] != '-')
	    {
	      rc = OPT_RC_INVALID;
	      printf ("Error: Invalid option string <%s>.\n", optionString);
	      break;
	    }

	  // Iterate through the singular option string, throw out
	  // garbage, duplicates and set flags to be used in formatting
	  for (y = 1; y < optionStringLength; y++)
	    {
	      switch (optionString[y])
		{
		  // Use absolute addressing
		case 'a':
		  SET_OPTION (blockOptions, BLOCK_ABSOLUTE, 'a');
		  break;

		  // Dump the binary contents of the page
		case 'b':
		  SET_OPTION (blockOptions, BLOCK_BINARY, 'b');
		  break;

		  // Dump the listed file as a control file
		case 'c':
		  SET_OPTION (controlOptions, CONTROL_DUMP, 'c');
		  break;

		  // Do not interpret the data. Format to hex and ascii.
		case 'd':
		  SET_OPTION (blockOptions, BLOCK_NO_INTR, 'd');
		  break;

		  // Format the contents of the block with interpretation
		  // of the headers
		case 'f':
		  SET_OPTION (blockOptions, BLOCK_FORMAT, 'f');
		  break;

		  // Display the usage screen
		case 'h':
		  rc = OPT_RC_COPYRIGHT;
		  break;

		  // Format the items in detail
		case 'i':
		  SET_OPTION (itemOptions, ITEM_DETAIL, 'i');
		  break;

		  // Interpret items as standard index values
		case 'x':
		  SET_OPTION (itemOptions, ITEM_INDEX, 'x');
		  if (itemOptions & ITEM_HEAP)
		    {
		      rc = OPT_RC_INVALID;
		      printf ("Error: Options <y> and <x> are "
			      "mutually exclusive.\n");
		    }
		  break;

		  // Interpret items as heap values
		case 'y':
		  SET_OPTION (itemOptions, ITEM_HEAP, 'y');
		  if (itemOptions & ITEM_INDEX)
		    {
		      rc = OPT_RC_INVALID;
		      printf ("Error: Options <x> and <y> are "
			      "mutually exclusive.\n");
		    }
		  break;

		default:
		  rc = OPT_RC_INVALID;
		  printf ("Error: Unknown option <%c>.\n", optionString[y]);
		  break;
		}

	      if (rc)
		break;
	    }
	}
    }

  if (rc == OPT_RC_DUPLICATE)
    printf ("Error: Duplicate option listed <%c>.\n", duplicateSwitch);

  // If the user requested a control file dump, a pure binary
  // block dump or a non-interpreted formatted dump, mask off
  // all other block level options (with a few exceptions)
  if (rc == OPT_RC_VALID)
    {
      // The user has requested a control file dump, only -f and
      // -S are valid... turn off all other formatting
      if (controlOptions & CONTROL_DUMP)
	{
	  if ((blockOptions & ~(BLOCK_FORMAT | BLOCK_FORCED))
	      || (itemOptions))
	    {
	      rc = OPT_RC_INVALID;
	      printf ("Error: Invalid options used for Control File dump.\n"
		      "       Only options <Sf> may be used with <c>.\n");
	    }
	  else
	    {
	      controlOptions |=
		(blockOptions & (BLOCK_FORMAT | BLOCK_FORCED));
	      blockOptions = itemOptions = 0;
	    }
	}
      // The user has requested a binary block dump... only -R and
      // -f are honoured
      else if (blockOptions & BLOCK_BINARY)
	{
	  blockOptions &= (BLOCK_BINARY | BLOCK_RANGE | BLOCK_FORCED);
	  itemOptions = 0;
	}
      // The user has requested a non-interpreted dump... only -a,
      // -R and -f are honoured
      else if (blockOptions & BLOCK_NO_INTR)
	{
	  blockOptions &=
	    (BLOCK_NO_INTR | BLOCK_ABSOLUTE | BLOCK_RANGE | BLOCK_FORCED);
	  itemOptions = 0;
	}
    }

  return (rc);
}

// Given the index into the parameter list, convert and return the
// current string to a number if possible
static int
GetOptionValue (char *optionString)
{
  unsigned int x;
  int value = -1;
  int optionStringLength = strlen (optionString);

  // Verify the next option looks like a number
  for (x = 0; x < optionStringLength; x++)
    if (!isdigit ((int) optionString[x]))
      break;

  // Convert the string to a number if it looks good
  if (x == optionStringLength)
    value = atoi (optionString);

  return (value);
}

// Read the page header off of block 0 to determine the block size
// used in this file.  Can be overridden using the -S option.  The
// returned value is the block size of block 0 on disk
static unsigned int
GetBlockSize ()
{
  unsigned int pageHeaderSize = sizeof (PageHeaderData);
  unsigned int localSize = 0;
  int bytesRead = 0;
  char localCache[pageHeaderSize];

  // Read the first header off of block 0 to determine the block size
  bytesRead = fread (&localCache, 1, pageHeaderSize, fp);
  rewind (fp);

  if (bytesRead == pageHeaderSize)
    localSize = (unsigned int) PageGetPageSize (&localCache);
  else
    printf ("Error: Unable to read full page header from block 0.\n"
	    "  ===> Read %u bytes", bytesRead);
  return (localSize);
}

// Determine the contents of the special section on the block and
// return this enum value
static unsigned int
GetSpecialSectionType (Page page)
{
  unsigned int rc;
  unsigned int specialOffset;
  unsigned int specialSize;
  unsigned int specialValue;
  PageHeader pageHeader = (PageHeader) page;

  // If this is not a partial header, check the validity of the
  // special section offset and contents
  if (bytesToFormat > sizeof (PageHeaderData))
    {
      specialOffset = (unsigned int) pageHeader->pd_special;

      // Check that the special offset can remain on the block or
      // the partial block
      if ((specialOffset == 0) ||
	  (specialOffset > blockSize) || (specialOffset > bytesToFormat))
	rc = SPEC_SECT_ERROR_BOUNDARY;
      else
	{
	  // we may need to examine last 2 bytes of page to identify index
	  uint16 *ptype = (uint16 *) (buffer + blockSize - sizeof(uint16));

	  specialSize = blockSize - specialOffset;

	  // If there is a special section, use its size to guess its
	  // contents, checking the last 2 bytes of the page in cases
	  // that are ambiguous.  Note we don't attempt to dereference
	  // the pointers without checking bytesToFormat == blockSize.
	  if (specialSize == 0)
	    rc = SPEC_SECT_NONE;
	  else if (specialSize == MAXALIGN (sizeof (uint32)))
	    {
	      // If MAXALIGN is 8, this could be either a sequence or
	      // SP-GiST or GIN.
	      if (bytesToFormat == blockSize)
		{
		  specialValue = *((int *) (buffer + specialOffset));
		  if (specialValue == SEQUENCE_MAGIC)
		    rc = SPEC_SECT_SEQUENCE;
		  else if (specialSize == MAXALIGN (sizeof (SpGistPageOpaqueData)) &&
			   *ptype == SPGIST_PAGE_ID)
		      rc = SPEC_SECT_INDEX_SPGIST;
		  else if (specialSize == MAXALIGN (sizeof (GinPageOpaqueData)))
		    rc = SPEC_SECT_INDEX_GIN;
		  else
		    rc = SPEC_SECT_ERROR_UNKNOWN;
		}
	      else
		rc = SPEC_SECT_ERROR_UNKNOWN;
	    }
	  // SP-GiST and GIN have same size special section, so check
	  // the page ID bytes first.
	  else if (specialSize == MAXALIGN (sizeof (SpGistPageOpaqueData)) &&
		   bytesToFormat == blockSize &&
		   *ptype == SPGIST_PAGE_ID)
	      rc = SPEC_SECT_INDEX_SPGIST;
	  else if (specialSize == MAXALIGN (sizeof (GinPageOpaqueData)))
	      rc = SPEC_SECT_INDEX_GIN;
	  else if (specialSize > 2 && bytesToFormat == blockSize)
	    {
	      // As of 8.3, BTree, Hash, and GIST all have the same size
	      // special section, but the last two bytes of the section
	      // can be checked to determine what's what.
	      if (*ptype <= MAX_BT_CYCLE_ID &&
		  specialSize == MAXALIGN (sizeof (BTPageOpaqueData)))
		rc = SPEC_SECT_INDEX_BTREE;
	      else if (*ptype == HASHO_PAGE_ID &&
		  specialSize == MAXALIGN (sizeof (HashPageOpaqueData)))
		rc = SPEC_SECT_INDEX_HASH;
	      else if (*ptype == GIST_PAGE_ID &&
		       specialSize == MAXALIGN (sizeof (GISTPageOpaqueData)))
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

// Check whether page is a btree meta page
static bool
IsBtreeMetaPage(Page page)
{
  PageHeader pageHeader = (PageHeader) page;

  if ((PageGetSpecialSize (page) == (MAXALIGN (sizeof (BTPageOpaqueData))))
      && (bytesToFormat == blockSize))
    {
      BTPageOpaque btpo =
	(BTPageOpaque) ((char *) page + pageHeader->pd_special);

      // Must check the cycleid to be sure it's really btree.
      if ((btpo->btpo_cycleid <= MAX_BT_CYCLE_ID) &&
	  (btpo->btpo_flags & BTP_META))
	return true;
    }
  return false;
}

// Display a header for the dump so we know the file name, the options
// used and the time the dump was taken
static void
CreateDumpFileHeader (int numOptions, char **options)
{
  unsigned int x;
  char optionBuffer[52] = "\0";
  time_t rightNow = time (NULL);

  // Iterate through the options and cache them.
  // The maximum we can display is 50 option characters + spaces.
  for (x = 1; x < (numOptions - 1); x++)
    {
      if ((strlen (optionBuffer) + strlen (options[x])) > 50)
	break;
      strcat (optionBuffer, options[x]);
      strcat (optionBuffer, " ");
    }

  printf
    ("\n*******************************************************************\n"
     "* PostgreSQL File/Block Formatted Dump Utility - Version %s\n"
     "*\n"
     "* File: %s\n"
     "* Options used: %s\n*\n"
     "* Dump created on: %s"
     "*******************************************************************\n",
     FD_VERSION, fileName, (strlen (optionBuffer)) ? optionBuffer : "None",
     ctime (&rightNow));
}

// Dump out a formatted block header for the requested block
static int
FormatHeader (Page page)
{
  int rc = 0;
  unsigned int headerBytes;
  PageHeader pageHeader = (PageHeader) page;

  printf ("<Header> -----\n");

  // Only attempt to format the header if the entire header (minus the item
  // array) is available
  if (bytesToFormat < offsetof (PageHeaderData, pd_linp[0]))
    {
      headerBytes = bytesToFormat;
      rc = EOF_ENCOUNTERED;
    }
  else
    {
      XLogRecPtr pageLSN = PageGetLSN (page);
      int maxOffset = PageGetMaxOffsetNumber (page);
      char flagString[100];

      headerBytes = offsetof (PageHeaderData, pd_linp[0]);
      blockVersion = (unsigned int) PageGetPageLayoutVersion (page);

      // The full header exists but we have to check that the item array
      // is available or how far we can index into it
      if (maxOffset > 0)
	{
	  unsigned int itemsLength = maxOffset * sizeof (ItemIdData);
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
	  strcat (flagString, "HAS_FREE_LINES|");
      if (pageHeader->pd_flags & PD_PAGE_FULL)
	  strcat (flagString, "PAGE_FULL|");
      if (pageHeader->pd_flags & PD_ALL_VISIBLE)
	  strcat (flagString, "ALL_VISIBLE|");
      if (strlen (flagString))
	  flagString[strlen (flagString) - 1] = '\0';

      // Interpret the content of the header
      printf
	(" Block Offset: 0x%08x         Offsets: Lower    %4u (0x%04hx)\n"
	 " Block: Size %4d  Version %4u            Upper    %4u (0x%04hx)\n"
	 " LSN:  logid %6d recoff 0x%08x      Special  %4u (0x%04hx)\n"
	 " Items: %4d                      Free Space: %4u\n"
	 " TLI: 0x%04x  Prune XID: 0x%08x  Flags: 0x%04x (%s)\n"
	 " Length (including item array): %u\n\n",
	 pageOffset, pageHeader->pd_lower, pageHeader->pd_lower,
	 (int) PageGetPageSize (page), blockVersion,
	 pageHeader->pd_upper, pageHeader->pd_upper,
	 pageLSN.xlogid, pageLSN.xrecoff,
	 pageHeader->pd_special, pageHeader->pd_special,
	 maxOffset, pageHeader->pd_upper - pageHeader->pd_lower,
	 pageHeader->pd_tli, pageHeader->pd_prune_xid,
	 pageHeader->pd_flags, flagString,
	 headerBytes);

      // If it's a btree meta page, print the contents of the meta block.
      if (IsBtreeMetaPage(page))
	{
	  BTMetaPageData *btpMeta = BTPageGetMeta (buffer);
	  printf (" BTree Meta Data:  Magic (0x%08x)   Version (%u)\n"
		  "                   Root:     Block (%u)  Level (%u)\n"
		  "                   FastRoot: Block (%u)  Level (%u)\n\n",
		  btpMeta->btm_magic, btpMeta->btm_version,
		  btpMeta->btm_root, btpMeta->btm_level,
		  btpMeta->btm_fastroot, btpMeta->btm_fastlevel);
	  headerBytes += sizeof (BTMetaPageData);
	}

      // Eye the contents of the header and alert the user to possible
      // problems.
      if ((maxOffset < 0) ||
	  (maxOffset > blockSize) ||
	  (blockVersion != PG_PAGE_LAYOUT_VERSION) || /* only one we support */
	  (pageHeader->pd_upper > blockSize) ||
	  (pageHeader->pd_upper > pageHeader->pd_special) ||
	  (pageHeader->pd_lower <
	   (sizeof (PageHeaderData) - sizeof (ItemIdData)))
	  || (pageHeader->pd_lower > blockSize)
	  || (pageHeader->pd_upper < pageHeader->pd_lower)
	  || (pageHeader->pd_special > blockSize))
	printf (" Error: Invalid header information.\n\n");
    }

  // If we have reached the end of file while interpreting the header, let
  // the user know about it
  if (rc == EOF_ENCOUNTERED)
    printf
      (" Error: End of block encountered within the header."
       " Bytes read: %4u.\n\n", bytesToFormat);

  // A request to dump the formatted binary of the block (header,
  // items and special section).  It's best to dump even on an error
  // so the user can see the raw image.
  if (blockOptions & BLOCK_FORMAT)
    FormatBinary (headerBytes, 0);

  return (rc);
}

// Dump out formatted items that reside on this block
static void
FormatItemBlock (Page page)
{
  unsigned int x;
  unsigned int itemSize;
  unsigned int itemOffset;
  unsigned int itemFlags;
  ItemId itemId;
  int maxOffset = PageGetMaxOffsetNumber (page);

  // If it's a btree meta page, the meta block is where items would normally
  // be; don't print garbage.
  if (IsBtreeMetaPage(page))
    return;

  printf ("<Data> ------ \n");

  // Loop through the items on the block.  Check if the block is
  // empty and has a sensible item array listed before running
  // through each item
  if (maxOffset == 0)
    printf (" Empty block - no items listed \n\n");
  else if ((maxOffset < 0) || (maxOffset > blockSize))
    printf (" Error: Item index corrupt on block. Offset: <%d>.\n\n",
	    maxOffset);
  else
    {
      int formatAs;
      char textFlags[16];

      // First, honour requests to format items a special way, then
      // use the special section to determine the format style
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
	  itemId = PageGetItemId (page, x);
	  itemFlags = (unsigned int) ItemIdGetFlags (itemId);
	  itemSize = (unsigned int) ItemIdGetLength (itemId);
	  itemOffset = (unsigned int) ItemIdGetOffset (itemId);

	  switch (itemFlags)
	  {
	    case LP_UNUSED:
	      strcpy (textFlags, "UNUSED");
	      break;
	    case LP_NORMAL:
	      strcpy (textFlags, "NORMAL");
	      break;
	    case LP_REDIRECT:
	      strcpy (textFlags, "REDIRECT");
	      break;
	    case LP_DEAD:
	      strcpy (textFlags, "DEAD");
	      break;
	    default:
	      // shouldn't be possible
	      sprintf (textFlags, "0x%02x", itemFlags);
	      break;
	  }

	  printf (" Item %3u -- Length: %4u  Offset: %4u (0x%04x)"
		  "  Flags: %s\n", x, itemSize, itemOffset, itemOffset,
		  textFlags);

	  // Make sure the item can physically fit on this block before
	  // formatting
	  if ((itemOffset + itemSize > blockSize) ||
	      (itemOffset + itemSize > bytesToFormat))
	    printf ("  Error: Item contents extend beyond block.\n"
		    "         BlockSize<%d> Bytes Read<%d> Item Start<%d>.\n",
		    blockSize, bytesToFormat, itemOffset + itemSize);
	  else
	    {
	      // If the user requests that the items be interpreted as
	      // heap or index items...
	      if (itemOptions & ITEM_DETAIL)
		FormatItem (itemSize, itemOffset, formatAs);

	      // Dump the items contents in hex and ascii
	      if (blockOptions & BLOCK_FORMAT)
		FormatBinary (itemSize, itemOffset);

	      if (x == maxOffset)
		printf ("\n");
	    }
	}
    }
}

// Interpret the contents of the item based on whether it has a special
// section and/or the user has hinted
static void
FormatItem (unsigned int numBytes, unsigned int startIndex,
	    unsigned int formatAs)
{
  static const char * const spgist_tupstates[4] = {
      "LIVE",
      "REDIRECT",
      "DEAD",
      "PLACEHOLDER"
  };

  if (formatAs == ITEM_INDEX)
    {
      // It is an IndexTuple item, so dump the index header
      if (numBytes < SizeOfIptrData)
	{
	  if (numBytes)
	    printf ("  Error: This item does not look like an index item.\n");
	}
      else
	{
	  IndexTuple itup = (IndexTuple) (&(buffer[startIndex]));
	  printf ("  Block Id: %u  linp Index: %u  Size: %d\n"
		  "  Has Nulls: %u  Has Varwidths: %u\n\n",
		  ((uint32) ((itup->t_tid.ip_blkid.bi_hi << 16) |
			     (uint16) itup->t_tid.ip_blkid.bi_lo)),
		  itup->t_tid.ip_posid,
		  (int) IndexTupleSize(itup),
		  IndexTupleHasNulls(itup) ? 1 : 0,
		  IndexTupleHasVarwidths(itup) ? 1 : 0);

	  if (numBytes != IndexTupleSize (itup))
	    printf ("  Error: Item size difference. Given <%u>, "
		    "Internal <%d>.\n", numBytes, (int) IndexTupleSize (itup));
	}
    }
  else if (formatAs == ITEM_SPG_INNER)
    {
      // It is an SpGistInnerTuple item, so dump the index header
      if (numBytes < SGITHDRSZ)
	{
	  if (numBytes)
	    printf ("  Error: This item does not look like an SPGiST item.\n");
	}
      else
	{
	  SpGistInnerTuple itup = (SpGistInnerTuple) (&(buffer[startIndex]));
	  printf ("  State: %s  allTheSame: %d nNodes: %u prefixSize: %u\n\n",
		  spgist_tupstates[itup->tupstate],
		  itup->allTheSame,
		  itup->nNodes,
		  itup->prefixSize);

	  if (numBytes != itup->size)
	    printf ("  Error: Item size difference. Given <%u>, "
		    "Internal <%d>.\n", numBytes, (int) itup->size);
	  else if (itup->prefixSize == MAXALIGN(itup->prefixSize))
	  {
	      int i;
	      SpGistNodeTuple node;

	      // Dump the prefix contents in hex and ascii
	      if ((blockOptions & BLOCK_FORMAT) &&
		  SGITHDRSZ + itup->prefixSize <= numBytes)
		  FormatBinary (SGITHDRSZ + itup->prefixSize, startIndex);

	      // Try to print the nodes, but only while pointer is sane
	      SGITITERATE(itup, i, node)
	      {
		  int off = (char *) node - (char *) itup;
		  if (off + SGNTHDRSZ > numBytes)
		      break;
		  printf ("  Node %2u:  Downlink: %u/%u  Size: %d  Null: %u\n",
			  i,
			  ((uint32) ((node->t_tid.ip_blkid.bi_hi << 16) |
				     (uint16) node->t_tid.ip_blkid.bi_lo)),
			  node->t_tid.ip_posid,
			  (int) IndexTupleSize(node),
			  IndexTupleHasNulls(node) ? 1 : 0);
		  // Dump the node's contents in hex and ascii
		  if ((blockOptions & BLOCK_FORMAT) &&
		      off + IndexTupleSize(node) <= numBytes)
		      FormatBinary (IndexTupleSize(node), startIndex + off);
		  if (IndexTupleSize(node) != MAXALIGN(IndexTupleSize(node)))
		      break;
	      }
	  }
	  printf ("\n");
	}
    }
  else if (formatAs == ITEM_SPG_LEAF)
    {
      // It is an SpGistLeafTuple item, so dump the index header
      if (numBytes < SGLTHDRSZ)
	{
	  if (numBytes)
	    printf ("  Error: This item does not look like an SPGiST item.\n");
	}
      else
	{
	  SpGistLeafTuple itup = (SpGistLeafTuple) (&(buffer[startIndex]));
	  printf ("  State: %s  nextOffset: %u  Block Id: %u  linp Index: %u\n\n",
		  spgist_tupstates[itup->tupstate],
		  itup->nextOffset,
		  ((uint32) ((itup->heapPtr.ip_blkid.bi_hi << 16) |
			     (uint16) itup->heapPtr.ip_blkid.bi_lo)),
		  itup->heapPtr.ip_posid);

	  if (numBytes != itup->size)
	    printf ("  Error: Item size difference. Given <%u>, "
		    "Internal <%d>.\n", numBytes, (int) itup->size);
	}
    }
  else
    {
      // It is a HeapTuple item, so dump the heap header
      int alignedSize = MAXALIGN (sizeof (HeapTupleHeaderData));

      if (numBytes < alignedSize)
	{
	  if (numBytes)
	    printf ("  Error: This item does not look like a heap item.\n");
	}
      else
	{
	  char flagString[256];
	  unsigned int x;
	  unsigned int bitmapLength = 0;
	  unsigned int oidLength = 0;
	  unsigned int computedLength;
	  unsigned int infoMask;
	  unsigned int infoMask2;
	  int localNatts;
	  unsigned int localHoff;
	  bits8 *localBits;
	  unsigned int localBitOffset;

	  HeapTupleHeader htup = (HeapTupleHeader) (&buffer[startIndex]);

	  infoMask = htup->t_infomask;
	  infoMask2 = htup->t_infomask2;
	  localBits = &(htup->t_bits[0]);
	  localNatts = HeapTupleHeaderGetNatts(htup);
	  localHoff = htup->t_hoff;
	  localBitOffset = offsetof (HeapTupleHeaderData, t_bits);

	  printf ("  XMIN: %u  XMAX: %u  CID|XVAC: %u",
		  HeapTupleHeaderGetXmin(htup),
		  HeapTupleHeaderGetXmax(htup),
		  HeapTupleHeaderGetRawCommandId(htup));

	  if (infoMask & HEAP_HASOID)
	    printf ("  OID: %u",
		    HeapTupleHeaderGetOid(htup));

	  printf ("\n"
		  "  Block Id: %u  linp Index: %u   Attributes: %d   Size: %d\n",
		  ((uint32)
		   ((htup->t_ctid.ip_blkid.bi_hi << 16) | (uint16) htup->
		    t_ctid.ip_blkid.bi_lo)), htup->t_ctid.ip_posid,
		  localNatts, htup->t_hoff);

	  // Place readable versions of the tuple info mask into a buffer.
	  // Assume that the string can not expand beyond 256.
	  flagString[0] = '\0';
	  if (infoMask & HEAP_HASNULL)
	    strcat (flagString, "HASNULL|");
	  if (infoMask & HEAP_HASVARWIDTH)
	    strcat (flagString, "HASVARWIDTH|");
	  if (infoMask & HEAP_HASEXTERNAL)
	    strcat (flagString, "HASEXTERNAL|");
	  if (infoMask & HEAP_HASOID)
	    strcat (flagString, "HASOID|");
	  if (infoMask & HEAP_COMBOCID)
	    strcat (flagString, "COMBOCID|");
	  if (infoMask & HEAP_XMAX_EXCL_LOCK)
	    strcat (flagString, "XMAX_EXCL_LOCK|");
	  if (infoMask & HEAP_XMAX_SHARED_LOCK)
	    strcat (flagString, "XMAX_SHARED_LOCK|");
	  if (infoMask & HEAP_XMIN_COMMITTED)
	    strcat (flagString, "XMIN_COMMITTED|");
	  if (infoMask & HEAP_XMIN_INVALID)
	    strcat (flagString, "XMIN_INVALID|");
	  if (infoMask & HEAP_XMAX_COMMITTED)
	    strcat (flagString, "XMAX_COMMITTED|");
	  if (infoMask & HEAP_XMAX_INVALID)
	    strcat (flagString, "XMAX_INVALID|");
	  if (infoMask & HEAP_XMAX_IS_MULTI)
	    strcat (flagString, "XMAX_IS_MULTI|");
	  if (infoMask & HEAP_UPDATED)
	    strcat (flagString, "UPDATED|");
	  if (infoMask & HEAP_MOVED_OFF)
	    strcat (flagString, "MOVED_OFF|");
	  if (infoMask & HEAP_MOVED_IN)
	    strcat (flagString, "MOVED_IN|");

	  if (infoMask2 & HEAP_HOT_UPDATED)
	    strcat (flagString, "HOT_UPDATED|");
	  if (infoMask2 & HEAP_ONLY_TUPLE)
	    strcat (flagString, "HEAP_ONLY|");

	  if (strlen (flagString))
	    flagString[strlen (flagString) - 1] = '\0';

	  printf ("  infomask: 0x%04x (%s) \n", infoMask, flagString);

	  // As t_bits is a variable length array, determine the length of
	  // the header proper
	  if (infoMask & HEAP_HASNULL)
	    bitmapLength = BITMAPLEN (localNatts);
	  else
	    bitmapLength = 0;

	  if (infoMask & HEAP_HASOID)
	    oidLength += sizeof (Oid);

	  computedLength =
	    MAXALIGN (localBitOffset + bitmapLength + oidLength);

	  // Inform the user of a header size mismatch or dump the t_bits array
	  if (computedLength != localHoff)
	    printf
	      ("  Error: Computed header length not equal to header size.\n"
	       "         Computed <%u>  Header: <%d>\n", computedLength,
	       localHoff);
	  else if ((infoMask & HEAP_HASNULL) && bitmapLength)
	    {
	      printf ("  t_bits: ");
	      for (x = 0; x < bitmapLength; x++)
		{
		  printf ("[%u]: 0x%02x ", x, localBits[x]);
		  if (((x & 0x03) == 0x03) && (x < bitmapLength - 1))
		    printf ("\n          ");
		}
	      printf ("\n");
	    }
	  printf ("\n");
	}
    }
}


// On blocks that have special sections, print the contents
// according to previously determined special section type
static void
FormatSpecial ()
{
  PageHeader pageHeader = (PageHeader) buffer;
  char flagString[100] = "\0";
  unsigned int specialOffset = pageHeader->pd_special;
  unsigned int specialSize =
    (blockSize >= specialOffset) ? (blockSize - specialOffset) : 0;

  printf ("<Special Section> -----\n");

  switch (specialType)
    {
    case SPEC_SECT_ERROR_UNKNOWN:
    case SPEC_SECT_ERROR_BOUNDARY:
      printf (" Error: Invalid special section encountered.\n");
      break;

    case SPEC_SECT_SEQUENCE:
      printf (" Sequence: 0x%08x\n", SEQUENCE_MAGIC);
      break;

      // Btree index section
    case SPEC_SECT_INDEX_BTREE:
      {
	BTPageOpaque btreeSection = (BTPageOpaque) (buffer + specialOffset);
	if (btreeSection->btpo_flags & BTP_LEAF)
	  strcat (flagString, "LEAF|");
	if (btreeSection->btpo_flags & BTP_ROOT)
	  strcat (flagString, "ROOT|");
	if (btreeSection->btpo_flags & BTP_DELETED)
	  strcat (flagString, "DELETED|");
	if (btreeSection->btpo_flags & BTP_META)
	  strcat (flagString, "META|");
	if (btreeSection->btpo_flags & BTP_HALF_DEAD)
	  strcat (flagString, "HALFDEAD|");
	if (btreeSection->btpo_flags & BTP_SPLIT_END)
	  strcat (flagString, "SPLITEND|");
	if (btreeSection->btpo_flags & BTP_HAS_GARBAGE)
	  strcat (flagString, "HASGARBAGE|");
	if (strlen (flagString))
	  flagString[strlen (flagString) - 1] = '\0';

	printf (" BTree Index Section:\n"
		"  Flags: 0x%04x (%s)\n"
		"  Blocks: Previous (%d)  Next (%d)  %s (%d)  CycleId (%d)\n\n",
		btreeSection->btpo_flags, flagString,
		btreeSection->btpo_prev, btreeSection->btpo_next,
		(btreeSection->
		 btpo_flags & BTP_DELETED) ? "Next XID" : "Level",
		btreeSection->btpo.level,
		btreeSection->btpo_cycleid);
      }
      break;

      // Hash index section
    case SPEC_SECT_INDEX_HASH:
      {
	HashPageOpaque hashSection = (HashPageOpaque) (buffer + specialOffset);
	if (hashSection->hasho_flag & LH_UNUSED_PAGE)
	  strcat (flagString, "UNUSED|");
	if (hashSection->hasho_flag & LH_OVERFLOW_PAGE)
	  strcat (flagString, "OVERFLOW|");
	if (hashSection->hasho_flag & LH_BUCKET_PAGE)
	  strcat (flagString, "BUCKET|");
	if (hashSection->hasho_flag & LH_BITMAP_PAGE)
	  strcat (flagString, "BITMAP|");
	if (hashSection->hasho_flag & LH_META_PAGE)
	  strcat (flagString, "META|");
	if (strlen (flagString))
	  flagString[strlen (flagString) - 1] = '\0';
	printf (" Hash Index Section:\n"
		"  Flags: 0x%04x (%s)\n"
		"  Bucket Number: 0x%04x\n"
		"  Blocks: Previous (%d)  Next (%d)\n\n",
		hashSection->hasho_flag, flagString,
		hashSection->hasho_bucket,
		hashSection->hasho_prevblkno, hashSection->hasho_nextblkno);
      }
      break;

      // GIST index section
    case SPEC_SECT_INDEX_GIST:
      {
	GISTPageOpaque gistSection = (GISTPageOpaque) (buffer + specialOffset);
	if (gistSection->flags & F_LEAF)
	  strcat (flagString, "LEAF|");
	if (gistSection->flags & F_DELETED)
	  strcat (flagString, "DELETED|");
	if (gistSection->flags & F_TUPLES_DELETED)
	  strcat (flagString, "TUPLES_DELETED|");
	if (gistSection->flags & F_FOLLOW_RIGHT)
	  strcat (flagString, "FOLLOW_RIGHT|");
	if (strlen (flagString))
	  flagString[strlen (flagString) - 1] = '\0';
	printf (" GIST Index Section:\n"
		"  NSN: 0x%08x/0x%08x\n"
		"  RightLink: %d\n"
		"  Flags: 0x%08x (%s)\n\n",
		gistSection->nsn.xlogid, gistSection->nsn.xrecoff,
		gistSection->rightlink,
		gistSection->flags, flagString);
      }
      break;

      // GIN index section
    case SPEC_SECT_INDEX_GIN:
      {
	GinPageOpaque ginSection = (GinPageOpaque) (buffer + specialOffset);
	if (ginSection->flags & GIN_DATA)
	  strcat (flagString, "DATA|");
	if (ginSection->flags & GIN_LEAF)
	  strcat (flagString, "LEAF|");
	if (ginSection->flags & GIN_DELETED)
	  strcat (flagString, "DELETED|");
	if (ginSection->flags & GIN_META)
	  strcat (flagString, "META|");
	if (ginSection->flags & GIN_LIST)
	  strcat (flagString, "LIST|");
	if (ginSection->flags & GIN_LIST_FULLROW)
	  strcat (flagString, "FULLROW|");
	if (strlen (flagString))
	  flagString[strlen (flagString) - 1] = '\0';
	printf (" GIN Index Section:\n"
		"  Flags: 0x%08x (%s)  Maxoff: %d\n"
		"  Blocks: RightLink (%d)\n\n",
		ginSection->flags, flagString,
		ginSection->maxoff,
		ginSection->rightlink);
      }
      break;

      // SP-GIST index section
    case SPEC_SECT_INDEX_SPGIST:
      {
	SpGistPageOpaque spgistSection = (SpGistPageOpaque) (buffer + specialOffset);
	if (spgistSection->flags & SPGIST_META)
	  strcat (flagString, "META|");
	if (spgistSection->flags & SPGIST_DELETED)
	  strcat (flagString, "DELETED|");
	if (spgistSection->flags & SPGIST_LEAF)
	  strcat (flagString, "LEAF|");
	if (spgistSection->flags & SPGIST_NULLS)
	  strcat (flagString, "NULLS|");
	if (strlen (flagString))
	  flagString[strlen (flagString) - 1] = '\0';
	printf (" SPGIST Index Section:\n"
		"  Flags: 0x%08x (%s)\n"
		"  nRedirection: %d\n"
		"  nPlaceholder: %d\n\n",
		spgistSection->flags, flagString,
		spgistSection->nRedirection,
		spgistSection->nPlaceholder);
      }
      break;

      // No idea what type of special section this is
    default:
      printf (" Unknown special section type. Type: <%u>.\n", specialType);
      break;
    }

  // Dump the formatted contents of the special section
  if (blockOptions & BLOCK_FORMAT)
    {
      if (specialType == SPEC_SECT_ERROR_BOUNDARY)
	printf (" Error: Special section points off page."
		" Unable to dump contents.\n");
      else
	FormatBinary (specialSize, specialOffset);
    }
}

// For each block, dump out formatted header and content information
static void
FormatBlock ()
{
  Page page = (Page) buffer;
  pageOffset = blockSize * currentBlock;
  specialType = GetSpecialSectionType (page);

  printf ("\nBlock %4u **%s***************************************\n",
	  currentBlock,
	  (bytesToFormat ==
	   blockSize) ? "***************" : " PARTIAL BLOCK ");

  // Either dump out the entire block in hex+acsii fashion or
  // interpret the data based on block structure
  if (blockOptions & BLOCK_NO_INTR)
    FormatBinary (bytesToFormat, 0);
  else
    {
      int rc;
      // Every block contains a header, items and possibly a special
      // section.  Beware of partial block reads though
      rc = FormatHeader (page);

      // If we didn't encounter a partial read in the header, carry on...
      if (rc != EOF_ENCOUNTERED)
	{
	  FormatItemBlock (page);

	  if (specialType != SPEC_SECT_NONE)
	    FormatSpecial ();
	}
    }
}

// Dump out the content of the PG control file
static void
FormatControl ()
{
  unsigned int localPgVersion = 0;
  unsigned int controlFileSize = 0;
  time_t cd_time;
  time_t cp_time;

  printf
    ("\n<pg_control Contents> *********************************************\n\n");

  // Check the version
  if (bytesToFormat >= offsetof (ControlFileData, catalog_version_no))
    localPgVersion = ((ControlFileData *) buffer)->pg_control_version;

  if (localPgVersion >= 72)
    controlFileSize = sizeof (ControlFileData);
  else
    {
      printf ("pg_filedump: pg_control version %u not supported.\n",
	      localPgVersion);
      return;
    }

  // Interpret the control file if it's all there
  if (bytesToFormat >= controlFileSize)
    {
      ControlFileData *controlData = (ControlFileData *) buffer;
      CheckPoint *checkPoint = &(controlData->checkPointCopy);
      pg_crc32 crcLocal;
      char *dbState;

      // Compute a local copy of the CRC to verify the one on disk
      INIT_CRC32 (crcLocal);
      COMP_CRC32 (crcLocal, buffer, offsetof(ControlFileData, crc));
      FIN_CRC32 (crcLocal);

      // Grab a readable version of the database state
      switch (controlData->state)
	{
	case DB_STARTUP:
	  dbState = "STARTUP";
	  break;
	case DB_SHUTDOWNED:
	  dbState = "SHUTDOWNED";
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

      printf ("                          CRC: %s\n"
	      "           pg_control Version: %u%s\n"
	      "              Catalog Version: %u\n"
	      "            System Identifier: " UINT64_FORMAT "\n"
	      "                        State: %s\n"
	      "                Last Mod Time: %s"
	      "       Last Checkpoint Record: Log File (%u) Offset (0x%08x)\n"
	      "   Previous Checkpoint Record: Log File (%u) Offset (0x%08x)\n"
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
	      "             TOAST Chunk Size: %u\n"
	      "   Date and Time Type Storage: %s\n\n",
	      EQ_CRC32 (crcLocal,
			controlData->crc) ? "Correct" : "Not Correct",
	      controlData->pg_control_version,
	      (controlData->pg_control_version == PG_CONTROL_VERSION ?
	       "" : " (Not Correct!)"),
	      controlData->catalog_version_no,
	      controlData->system_identifier,
	      dbState,
	      ctime (&(cd_time)),
	      controlData->checkPoint.xlogid, controlData->checkPoint.xrecoff,
	      controlData->prevCheckPoint.xlogid, controlData->prevCheckPoint.xrecoff,
	      checkPoint->redo.xlogid, checkPoint->redo.xrecoff,
	      checkPoint->ThisTimeLineID,
	      checkPoint->nextXidEpoch, checkPoint->nextXid,
	      checkPoint->nextOid,
	      checkPoint->nextMulti, checkPoint->nextMultiOffset,
	      ctime (&cp_time),
	      controlData->minRecoveryPoint.xlogid, controlData->minRecoveryPoint.xrecoff,
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
	      controlData->toast_max_chunk_size,
	      (controlData->enableIntTimes ?
	       "64 bit Integers" : "Floating Point"));
    }
  else
    {
      printf (" Error: pg_control file size incorrect.\n"
	      "        Size: Correct <%u>  Received <%u>.\n\n",
	      controlFileSize, bytesToFormat);

      // If we have an error, force a formatted dump so we can see
      // where things are going wrong
      controlOptions |= CONTROL_FORMAT;
    }

  // Dump hex and ascii representation of data
  if (controlOptions & CONTROL_FORMAT)
    {
      printf ("<pg_control Formatted Dump> *****************"
	      "**********************\n\n");
      FormatBinary (bytesToFormat, 0);
    }
}

// Dump out the contents of the block in hex and ascii.
// BYTES_PER_LINE bytes are formatted in each line.
static void
FormatBinary (unsigned int numBytes, unsigned int startIndex)
{
  unsigned int index = 0;
  unsigned int stopIndex = 0;
  unsigned int x = 0;
  unsigned int lastByte = startIndex + numBytes;

  if (numBytes)
    {
      // Iterate through a printable row detailing the current
      // address, the hex and ascii values
      for (index = startIndex; index < lastByte; index += BYTES_PER_LINE)
	{
	  stopIndex = index + BYTES_PER_LINE;

	  // Print out the address
	  if (blockOptions & BLOCK_ABSOLUTE)
	    printf ("  %08x: ", (unsigned int) (pageOffset + index));
	  else
	    printf ("  %04x: ", (unsigned int) index);

	  // Print out the hex version of the data
	  for (x = index; x < stopIndex; x++)
	    {
	      if (x < lastByte)
		printf ("%02x", 0xff & ((unsigned) buffer[x]));
	      else
		printf ("  ");
	      if ((x & 0x03) == 0x03)
		printf (" ");
	    }
	  printf (" ");

	  // Print out the ascii version of the data
	  for (x = index; x < stopIndex; x++)
	    {
	      if (x < lastByte)
		printf ("%c", isprint (buffer[x]) ? buffer[x] : '.');
	      else
		printf (" ");
	    }
	  printf ("\n");
	}
      printf ("\n");
    }
}

// Dump the binary image of the block
static void
DumpBinaryBlock ()
{
  unsigned int x;
  for (x = 0; x < bytesToFormat; x++)
    putchar (buffer[x]);
}

// Control the dumping of the blocks within the file
static void
DumpFileContents ()
{
  unsigned int initialRead = 1;
  unsigned int contentsToDump = 1;

  // If the user requested a block range, seek to the correct position
  // within the file for the start block.
  if (blockOptions & BLOCK_RANGE)
    {
      unsigned int position = blockSize * blockStart;
      if (fseek (fp, position, SEEK_SET) != 0)
	{
	  printf ("Error: Seek error encountered before requested "
		  "start block <%d>.\n", blockStart);
	  contentsToDump = 0;
	}
      else
	currentBlock = blockStart;
    }

  // Iterate through the blocks in the file until you reach the end or
  // the requested range end
  while (contentsToDump)
    {
      bytesToFormat = fread (buffer, 1, blockSize, fp);

      if (bytesToFormat == 0)
	{
	  // fseek() won't pop an error if you seek passed eof.  The next
	  // subsequent read gets the error.
	  if (initialRead)
	    printf ("Error: Premature end of file encountered.\n");
	  else if (!(blockOptions & BLOCK_BINARY))
	    printf ("\n*** End of File Encountered. Last Block "
		    "Read: %d ***\n", currentBlock - 1);

	  contentsToDump = 0;
	}
      else
	{
	  if (blockOptions & BLOCK_BINARY)
	    DumpBinaryBlock ();
	  else
	    {
	      if (controlOptions & CONTROL_DUMP)
		{
		  FormatControl ();
		  contentsToDump = false;
		}
	      else
		FormatBlock ();
	    }
	}

      // Check to see if we are at the end of the requested range.
      if ((blockOptions & BLOCK_RANGE) &&
	  (currentBlock >= blockEnd) && (contentsToDump))
	{
	  //Don't print out message if we're doing a binary dump
	  if (!(blockOptions & BLOCK_BINARY))
	    printf ("\n*** End of Requested Range Encountered. "
		    "Last Block Read: %d ***\n", currentBlock);
	  contentsToDump = 0;
	}
      else
	currentBlock++;

      initialRead = 0;
    }
}

// Consume the options and iterate through the given file, formatting as
// requested.
int
main (int argv, char **argc)
{
  // If there is a parameter list, validate the options
  unsigned int validOptions;
  validOptions = (argv < 2) ? OPT_RC_COPYRIGHT : ConsumeOptions (argv, argc);

  // Display valid options if no parameters are received or invalid options
  // where encountered
  if (validOptions != OPT_RC_VALID)
    DisplayOptions (validOptions);
  else
    {
      // Don't dump the header if we're dumping binary pages
      if (!(blockOptions & BLOCK_BINARY))
	CreateDumpFileHeader (argv, argc);

      // If the user has not forced a block size, use the size of the
      // control file data or the information from the block 0 header
      if (controlOptions)
	{
	  if (!(controlOptions & CONTROL_FORCED))
	    blockSize = sizeof (ControlFileData);
	}
      else if (!(blockOptions & BLOCK_FORCED))
	blockSize = GetBlockSize ();

      // On a positive block size, allocate a local buffer to store
      // the subsequent blocks
      if (blockSize > 0)
	{
	  buffer = (char *) malloc (blockSize);
	  if (buffer)
	    DumpFileContents ();
	  else
	    printf ("\nError: Unable to create buffer of size <%d>.\n",
		    blockSize);
	}
    }

  // Close out the file and get rid of the allocated block buffer
  if (fp)
    fclose (fp);

  if (buffer)
    free (buffer);

  exit (0);
}
