/*
 * Code mostly borrowed from PostgreSQL's stringinfo.c
 * palloc replaced to malloc, etc.
 */

#include "postgres.h"
#include <lib/stringinfo.h>
#include <string.h>
#include <assert.h>

#define MaxAllocSize	((Size) 0x3fffffff) /* 1 gigabyte - 1 */

/*-------------------------
 * StringInfoData holds information about an extensible string.
 *	  data	  is the current buffer for the string.
 *	  len	  is the current string length.  There is guaranteed to be
 *			  a terminating '\0' at data[len], although this is not very
 *			  useful when the string holds binary data rather than text.
 *	  maxlen  is the allocated size in bytes of 'data', i.e. the maximum
 *			  string size (including the terminating '\0' char) that we can
 *			  currently store in 'data' without having to reallocate
 *			  more space.  We must always have maxlen > len.
 *	  cursor  is initialized to zero by makeStringInfo or initStringInfo,
 *			  but is not otherwise touched by the stringinfo.c routines.
 *			  Some routines use it to scan through a StringInfo.
 *-------------------------
 */

/*
 * initStringInfo
 *
 * Initialize a StringInfoData struct (with previously undefined contents)
 * to describe an empty string.
 */
void
initStringInfo(StringInfo str)
{
	int			size = 1024;	/* initial default buffer size */

	str->data = (char *) malloc(size);
	str->maxlen = size;
	resetStringInfo(str);
}

/*
 * resetStringInfo
 *
 * Reset the StringInfo: the data buffer remains valid, but its
 * previous content, if any, is cleared.
 */
void
resetStringInfo(StringInfo str)
{
	str->data[0] = '\0';
	str->len = 0;
	str->cursor = 0;
}

/*
 * appendStringInfoString
 *
 * Append a null-terminated string to str.
 */
void
appendStringInfoString(StringInfo str, const char *s)
{
	appendBinaryStringInfo(str, s, strlen(s));
}

/*
 * appendBinaryStringInfo
 *
 * Append arbitrary binary data to a StringInfo, allocating more space
 * if necessary.
 */
void
#if PG_VERSION_NUM < 160000
appendBinaryStringInfo(StringInfo str, const char *data, int datalen)
#else
appendBinaryStringInfo(StringInfo str, const void *data, int datalen)
#endif
{
	assert(str != NULL);

	/* Make more room if needed */
	enlargeStringInfo(str, datalen);

	/* OK, append the data */
	memcpy(str->data + str->len, data, datalen);
	str->len += datalen;

	/*
	 * Keep a trailing null in place, even though it's probably useless for
	 * binary data.  (Some callers are dealing with text but call this because
	 * their input isn't null-terminated.)
	 */
	str->data[str->len] = '\0';
}

/*
 * enlargeStringInfo
 *
 * Make sure there is enough space for 'needed' more bytes
 * ('needed' does not include the terminating null).
 *
 * External callers usually need not concern themselves with this, since
 * all stringinfo.c routines do it automatically.  However, if a caller
 * knows that a StringInfo will eventually become X bytes large, it
 * can save some malloc overhead by enlarging the buffer before starting
 * to store data in it.
 */
void
enlargeStringInfo(StringInfo str, int needed)
{
	Size		newlen;
	Size		limit;
	char	   *old_data;

	limit = MaxAllocSize;

	/*
	 * Guard against out-of-range "needed" values.  Without this, we can get
	 * an overflow or infinite loop in the following.
	 */
	if (needed < 0)				/* should not happen */
	{
		printf("Error: invalid string enlargement request size: %d", needed);
		exit(1);
	}

	if (((Size) needed) >= (limit - (Size) str->len))
	{
		printf("Error: cannot enlarge string buffer containing %d bytes by %d more bytes.",
			   str->len, needed);
		exit(1);
	}

	needed += str->len + 1;		/* total space required now */

	/* Because of the above test, we now have needed <= limit */

	if (needed <= str->maxlen)
		return;					/* got enough space already */

	/*
	 * We don't want to allocate just a little more space with each append;
	 * for efficiency, double the buffer size each time it overflows.
	 * Actually, we might need to more than double it if 'needed' is big...
	 */
	newlen = 2 * str->maxlen;
	while (needed > newlen)
		newlen = 2 * newlen;

	/*
	 * Clamp to the limit in case we went past it.  Note we are assuming here
	 * that limit <= INT_MAX/2, else the above loop could overflow.  We will
	 * still have newlen >= needed.
	 */
	if (newlen > limit)
		newlen = limit;

	old_data = str->data;
	str->data = (char *) realloc(str->data, (Size) newlen);
	if (str->data == NULL)
	{
		free(old_data);
		printf("Error: realloc() failed!\n");
		exit(1);
	}

	str->maxlen = newlen;
}
