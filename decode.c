#include "postgres.h"
#include "decode.h"
#include "pg_filedump.h"
#include <lib/stringinfo.h>
#include <access/htup_details.h>
#include <access/tupmacs.h>
#if PG_VERSION_NUM >= 140000
#ifdef USE_LZ4
#include <lz4.h>
#endif
#include <access/toast_compression.h>
#include <access/toast_internals.h>
#endif
#if PG_VERSION_NUM >= 130000
#include <access/detoast.h>
#include <access/heaptoast.h>
#else
#include <access/tuptoaster.h>
#endif
#include <datatype/timestamp.h>
#include <common/pg_lzcompress.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <assert.h>

#define ATTRTYPES_STR_MAX_LEN (1024-1)

static int
ReadStringFromToast(const char *buffer,
		unsigned int buff_size,
		unsigned int* out_size,
		int (*parse_value)(const char *, int));

/*
 * Utilities for manipulation of header information for compressed
 * toast entries.
 */
#if PG_VERSION_NUM < 140000
/*
 * These macros define the "saved size" portion of va_extinfo.  Its remaining
 * two high-order bits identify the compression method.
 * Before std14 only pglz compression method existed (with 00 bits).
 */
#define VARLENA_EXTSIZE_BITS   30
#define VARLENA_EXTSIZE_MASK   ((1U << VARLENA_EXTSIZE_BITS) - 1)
#define VARDATA_COMPRESSED_GET_COMPRESS_METHOD(ptr) ((*((uint32 *)ptr + 1)) >> VARLENA_EXTSIZE_BITS)
typedef enum ToastCompressionId
{
	TOAST_PGLZ_COMPRESSION_ID = 0,
	TOAST_LZ4_COMPRESSION_ID = 1,
	TOAST_INVALID_COMPRESSION_ID = 2
} ToastCompressionId;
#endif
#define TOAST_COMPRESS_RAWSIZE(ptr) ((*(uint32 *) ptr) & VARLENA_EXTSIZE_MASK)
#define TOAST_COMPRESS_RAWMETHOD(ptr) ((*(uint32 *) ptr) >> VARLENA_EXTSIZE_BITS)
#define TOAST_COMPRESS_RAWDATA(ptr) (ptr + sizeof(uint32))
#define TOAST_COMPRESS_HEADER_SIZE (sizeof(uint32))

typedef int (*decode_callback_t) (const char *buffer, unsigned int buff_size,
								  unsigned int *out_size);

static int
decode_smallint(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_int(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_uint(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_bigint(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_time(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_timetz(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_date(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_timestamp_internal(const char *buffer, unsigned int buff_size, unsigned int *out_size, bool with_timezone);

static int
decode_timestamp(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_timestamptz(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_float4(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_float8(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_bool(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_uuid(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_macaddr(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_string(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_char(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_name(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_numeric(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
extract_data(const char *buffer, unsigned int buff_size, unsigned int *out_size, int (*parse_value)(const char *, int));

static int
decode_ignore(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int	ncallbacks = 0;
static decode_callback_t callbacks[ATTRTYPES_STR_MAX_LEN / 2] =
{
	NULL
};

typedef struct
{
	char	   *name;
	decode_callback_t callback;
}			ParseCallbackTableItem;

static ParseCallbackTableItem callback_table[] =
{
	{
		"smallserial", &decode_smallint
	},
	{
		"smallint", &decode_smallint
	},
	{
		"int", &decode_int
	},
	{
		"oid", &decode_uint
	},
	{
		"xid", &decode_uint
	},
	{
		"serial", &decode_int
	},
	{
		"bigint", &decode_bigint
	},
	{
		"bigserial", &decode_bigint
	},
	{
		"time", &decode_time
	},
	{
		"timetz", &decode_timetz
	},
	{
		"date", &decode_date
	},
	{
		"timestamp", &decode_timestamp
	},
	{
		"timestamptz", &decode_timestamptz
	},
	{
		"real", &decode_float4
	},
	{
		"float4", &decode_float4
	},
	{
		"float8", &decode_float8
	},
	{
		"float", &decode_float8
	},
	{
		"bool", &decode_bool
	},
	{
		"uuid", &decode_uuid
	},
	{
		"macaddr", &decode_macaddr
	},
	{
		"name", &decode_name
	},
	{
		"numeric", &decode_numeric
	},
	{
		"char", &decode_char
	},
	{
		"~", &decode_ignore
	},

	/* internally all string types are stored the same way */
	{
		"charn", &decode_string
	},
	{
		"varchar", &decode_string
	},
	{
		"varcharn", &decode_string
	},
	{
		"text", &decode_string
	},
	{
		"json", &decode_string
	},
	{
		"xml", &decode_string
	},
	{
		NULL, NULL
	},
};

static StringInfoData copyString;
static bool copyStringInitDone = false;

/* Used by some PostgreSQL macro definitions */
#if PG_VERSION_NUM < 160000
void
ExceptionalCondition(const char *conditionName,
					 const char *errorType,
					 const char *fileName,
					 int lineNumber)
{
	printf("Exceptional condition: name = %s, type = %s, fname = %s, line = %d\n",
		   conditionName ? conditionName : "(NULL)",
		   errorType ? errorType : "(NULL)",
		   fileName ? fileName : "(NULL)",
		   lineNumber);
	exit(1);
}
#else
void
ExceptionalCondition(const char *conditionName,
					 const char *fileName,
					 int lineNumber)
{
	printf("Exceptional condition: name = %s, type = FailedAssertion, fname = %s, line = %d\n",
		   conditionName ? conditionName : "(NULL)",
		   fileName ? fileName : "(NULL)",
		   lineNumber);
	exit(1);
}
#endif

/* Append given string to current COPY line */
static void
CopyAppend(const char *str)
{
	if (!copyStringInitDone)
	{
		initStringInfo(&copyString);
		copyStringInitDone = true;
	}

	/* Caller probably wanted just to init copyString */
	if (str == NULL)
		return;

	if (copyString.data[0] != '\0')
		appendStringInfoString(&copyString, "\t");

	appendStringInfoString(&copyString, str);
}

/*
 * Append given string to current COPY line and encode special symbols
 * like \r, \n, \t and \\.
 */
static int
CopyAppendEncode(const char *str, int orig_len)
{
	int			curr_offset = 0;
	int			len = orig_len;
	char	   *tmp_buff = malloc(2 * orig_len + 1);

	if (tmp_buff == NULL)
	{
		perror("malloc");
		exit(1);
	}

	while (len > 0)
	{
		/*
		 * Since we are working with potentially corrupted data we can
		 * encounter \0 as well.
		 */
		if (*str == '\0')
		{
			tmp_buff[curr_offset] = '\\';
			tmp_buff[curr_offset + 1] = '0';
			curr_offset += 2;
		}
		else if (*str == '\r')
		{
			tmp_buff[curr_offset] = '\\';
			tmp_buff[curr_offset + 1] = 'r';
			curr_offset += 2;
		}
		else if (*str == '\n')
		{
			tmp_buff[curr_offset] = '\\';
			tmp_buff[curr_offset + 1] = 'n';
			curr_offset += 2;
		}
		else if (*str == '\t')
		{
			tmp_buff[curr_offset] = '\\';
			tmp_buff[curr_offset + 1] = 'r';
			curr_offset += 2;
		}
		else if (*str == '\\')
		{
			tmp_buff[curr_offset] = '\\';
			tmp_buff[curr_offset + 1] = '\\';
			curr_offset += 2;
		}
		else
		{
			/* It's a regular symbol. */
			tmp_buff[curr_offset] = *str;
			curr_offset++;
		}

		str++;
		len--;
	}

	tmp_buff[curr_offset] = '\0';
	CopyAppend(tmp_buff);
	free(tmp_buff);

	return 0;
}

/* CopyAppend version with format string support */
#define CopyAppendFmt(fmt, ...) do { \
	  char __copy_format_buff[512]; \
	  snprintf(__copy_format_buff, sizeof(__copy_format_buff), fmt, ##__VA_ARGS__); \
	  CopyAppend(__copy_format_buff); \
  } while(0)

/*
 * Decode a numeric type and append the result to current COPY line
 */
static int
CopyAppendNumeric(const char *buffer, int num_size)
{
	struct NumericData *num = (struct NumericData *) malloc(num_size);

	if (num == NULL)
		return -2;

	memcpy((char *) num, buffer, num_size);

	if (NUMERIC_IS_SPECIAL(num))
	{
		int	result = -2;

		if (NUMERIC_IS_NINF(num))
		{
			CopyAppend("-Infinity");
			result = 0;
		}
		if (NUMERIC_IS_PINF(num))
		{
			CopyAppend("Infinity");
			result = 0;
		}
		if (NUMERIC_IS_NAN(num))
		{
			CopyAppend("NaN");
			result = 0;
		}

		free(num);

		return result;
	}
	else
	{
		int			sign;
		int			weight;
		int			dscale;
		int			ndigits;
		int			i;
		char	   *str;
		char	   *cp;
		char	   *endcp;
		int			d;
		bool		putit;
		NumericDigit d1;
		NumericDigit dig;
		NumericDigit *digits;

		sign = NUMERIC_SIGN(num);
		weight = NUMERIC_WEIGHT(num);
		dscale = NUMERIC_DSCALE(num);

		if (num_size == NUMERIC_HEADER_SIZE(num))
		{
			/* No digits - compressed zero. */
			CopyAppendFmt("%d", 0);
			free(num);
			return 0;
		}
		else
		{
			ndigits = num_size / sizeof(NumericDigit);
			digits = (NumericDigit *) ((char *) num + NUMERIC_HEADER_SIZE(num));
			i = (weight + 1) * DEC_DIGITS;
			if (i <= 0)
				i = 1;

			str = palloc(i + dscale + DEC_DIGITS + 2);
			cp = str;

			/*
			 * Output a dash for negative values
			 */
			if (sign == NUMERIC_NEG)
				*cp++ = '-';

			/*
			 * Output all digits before the decimal point
			 */
			if (weight < 0)
			{
				d = weight + 1;
				*cp++ = '0';
			}
			else
			{
				for (d = 0; d <= weight; d++)
				{
					dig = (d < ndigits) ? digits[d] : 0;

					/*
					 * In the first digit, suppress extra leading decimal
					 * zeroes
					 */
					putit = (d > 0);
					d1 = dig / 1000;
					dig -= d1 * 1000;
					putit |= (d1 > 0);
					if (putit)
						*cp++ = d1 + '0';
					d1 = dig / 100;
					dig -= d1 * 100;
					putit |= (d1 > 0);
					if (putit)
						*cp++ = d1 + '0';
					d1 = dig / 10;
					dig -= d1 * 10;
					putit |= (d1 > 0);
					if (putit)
						*cp++ = d1 + '0';
					*cp++ = dig + '0';
				}
			}

			/*
			 * If requested, output a decimal point and all the digits that
			 * follow it. We initially put out a multiple of DEC_DIGITS
			 * digits, then truncate if needed.
			 */
			if (dscale > 0)
			{
				*cp++ = '.';
				endcp = cp + dscale;
				for (i = 0; i < dscale; d++, i += DEC_DIGITS)
				{
					dig = (d >= 0 && d < ndigits) ? digits[d] : 0;
					d1 = dig / 1000;
					dig -= d1 * 1000;
					*cp++ = d1 + '0';
					d1 = dig / 100;
					dig -= d1 * 100;
					*cp++ = d1 + '0';
					d1 = dig / 10;
					dig -= d1 * 10;
					*cp++ = d1 + '0';
					*cp++ = dig + '0';
				}
				cp = endcp;
			}
			*cp = '\0';
			CopyAppend(str);
			pfree(str);
			free(num);
			return 0;
		}
	}
}

/* Discard accumulated COPY line */
static void
CopyClear(void)
{
	/* Make sure init is done */
	CopyAppend(NULL);

	resetStringInfo(&copyString);
}

/* Output and then clear accumulated COPY line */
static void
CopyFlush(void)
{
	/* Make sure init is done */
	CopyAppend(NULL);

	printf("COPY: %s\n", copyString.data);
	CopyClear();
}

/*
 * Add a callback to `callbacks` table for given type name
 *
 * Arguments:
 *   type	   - name of a single type, always lowercase
 *
 * Return value is:
 *   == 0	   - no error
 *	< 0	   - invalid type name
 */
static int
AddTypeCallback(const char *type)
{
	int			idx = 0;

	if (*type == '\0')			/* ignore empty strings */
		return 0;

	while (callback_table[idx].name != NULL)
	{
		if (strcmp(callback_table[idx].name, type) == 0)
		{
			callbacks[ncallbacks] = callback_table[idx].callback;
			ncallbacks++;
			return 0;
		}
		idx++;
	}

	printf("Error: type <%s> doesn't exist or is not currently supported\n", type);
	printf("Full list of known types: ");
	idx = 0;
	while (callback_table[idx].name != NULL)
	{
		printf("%s ", callback_table[idx].name);
		idx++;
	}
	printf("\n");
	return -1;
}

/*
 * Decode attribute types string like "int,timestamp,bool,uuid"
 *
 * Arguments:
 *   str		- types string
 * Return value is:
 *   == 0	   - if string is valid
 *	< 0	   - if string is invalid
 */
int
ParseAttributeTypesString(const char *str)
{
	char	   *curr_type,
			   *next_type;
	char		attrtypes[ATTRTYPES_STR_MAX_LEN + 1];
	int			i,
				len = strlen(str);

	if (len > ATTRTYPES_STR_MAX_LEN)
	{
		printf("Error: attribute types string is longer then %u characters!\n",
			   ATTRTYPES_STR_MAX_LEN);
		return -1;
	}

	strcpy(attrtypes, str);
	for (i = 0; i < len; i++)
		attrtypes[i] = tolower(attrtypes[i]);

	curr_type = attrtypes;
	while (curr_type)
	{
		next_type = strstr(curr_type, ",");
		if (next_type)
		{
			*next_type = '\0';
			next_type++;
		}

		if (AddTypeCallback(curr_type) < 0)
			return -1;

		curr_type = next_type;
	}

	return 0;
}

/*
 * Convert Julian day number (JDN) to a date.
 * Copy-pasted from src/backend/utils/adt/datetime.c
 */
static void
j2date(int jd, int *year, int *month, int *day)
{
	unsigned int julian;
	unsigned int quad;
	unsigned int extra;
	int			y;

	julian = jd;
	julian += 32044;
	quad = julian / 146097;
	extra = (julian - quad * 146097) * 4 + 3;
	julian += 60 + quad * 3 + extra / 146097;
	quad = julian / 1461;
	julian -= quad * 1461;
	y = julian * 4 / 1461;
	julian = ((y != 0) ? ((julian + 305) % 365) : ((julian + 306) % 366))
		+ 123;
	y += quad * 4;
	*year = y - 4800;
	quad = julian * 2141 / 65536;
	*day = julian - 7834 * quad / 256;
	*month = (quad + 10) % MONTHS_PER_YEAR + 1;
}

/* Decode a smallint type */
static int
decode_smallint(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) SHORTALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int16))
		return -2;

	CopyAppendFmt("%d", (int) (*(int16 *) buffer));
	*out_size = sizeof(int16) + delta;
	return 0;
}


/* Decode an int type */
static int
decode_int(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) INTALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int32))
		return -2;

	CopyAppendFmt("%d", *(int32 *) buffer);
	*out_size = sizeof(int32) + delta;
	return 0;
}

/* Decode an unsigned int type */
static int
decode_uint(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) INTALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(uint32))
		return -2;

	CopyAppendFmt("%u", *(uint32 *) buffer);
	*out_size = sizeof(uint32) + delta;
	return 0;
}

/* Decode a bigint type */
static int
decode_bigint(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) LONGALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int64))
		return -2;

	CopyAppendFmt(INT64_FORMAT, *(int64 *) buffer);
	*out_size = sizeof(int64) + delta;
	return 0;
}

/* Decode a time type */
static int
decode_time(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) LONGALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);
	int64		timestamp,
				timestamp_sec;

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int64))
		return -2;

	timestamp = *(int64 *) buffer;
	timestamp_sec = timestamp / 1000000;
	*out_size = sizeof(int64) + delta;

	CopyAppendFmt("%02" INT64_MODIFIER "d:%02" INT64_MODIFIER "d:%02" INT64_MODIFIER "d.%06" INT64_MODIFIER "d",
				  timestamp_sec / 60 / 60, (timestamp_sec / 60) % 60, timestamp_sec % 60,
				  timestamp % 1000000);

	return 0;
}

/* Decode a timetz type */
static int
decode_timetz(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) LONGALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);
	int64		timestamp,
				timestamp_sec;
	int32		tz_sec,
				tz_min;

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < (sizeof(int64) + sizeof(int32)))
		return -2;

	timestamp = *(int64 *) buffer;
	tz_sec = *(int32 *) (buffer + sizeof(int64));
	timestamp_sec = timestamp / 1000000;
	tz_min = -(tz_sec / 60);
	*out_size = sizeof(int64) + sizeof(int32) + delta;

	CopyAppendFmt("%02" INT64_MODIFIER "d:%02" INT64_MODIFIER "d:%02" INT64_MODIFIER "d.%06" INT64_MODIFIER "d%c%02d:%02d",
				  timestamp_sec / 60 / 60, (timestamp_sec / 60) % 60, timestamp_sec % 60,
				  timestamp % 1000000, (tz_min > 0 ? '+' : '-'), abs(tz_min / 60), abs(tz_min % 60));

	return 0;
}

/* Decode a date type */
static int
decode_date(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) INTALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);
	int32		d,
				jd,
				year,
				month,
				day;

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int32))
		return -2;

	*out_size = sizeof(int32) + delta;

	d = *(int32 *) buffer;
	if (d == PG_INT32_MIN)
	{
		CopyAppend("-infinity");
		return 0;
	}
	if (d == PG_INT32_MAX)
	{
		CopyAppend("infinity");
		return 0;
	}

	jd = d + POSTGRES_EPOCH_JDATE;
	j2date(jd, &year, &month, &day);

	CopyAppendFmt("%04d-%02d-%02d%s", (year <= 0) ? -year + 1 : year, month, day, (year <= 0) ? " BC" : "");

	return 0;
}

/* Decode a timestamp type */
static int
decode_timestamp_internal(const char *buffer, unsigned int buff_size, unsigned int *out_size, bool with_timezone)
{
	const char *new_buffer = (const char *) LONGALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);
	int64		timestamp,
				timestamp_sec;
	int32		jd,
				year,
				month,
				day;

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int64))
		return -2;

	*out_size = sizeof(int64) + delta;
	timestamp = *(int64 *) buffer;

	if (timestamp == DT_NOBEGIN)
	{
		CopyAppend("-infinity");
		return 0;
	}
	if (timestamp == DT_NOEND)
	{
		CopyAppend("infinity");
		return 0;
	}

	jd = timestamp / USECS_PER_DAY;
	if (jd != 0)
		timestamp -= jd * USECS_PER_DAY;

	if (timestamp < INT64CONST(0))
	{
		timestamp += USECS_PER_DAY;
		jd -= 1;
	}

	/* add offset to go from J2000 back to standard Julian date */
	jd += POSTGRES_EPOCH_JDATE;

	j2date(jd, &year, &month, &day);
	timestamp_sec = timestamp / 1000000;

	CopyAppendFmt("%04d-%02d-%02d %02" INT64_MODIFIER "d:%02" INT64_MODIFIER "d:%02" INT64_MODIFIER "d.%06" INT64_MODIFIER "d%s%s",
				  (year <= 0) ? -year + 1 : year, month, day,
				  timestamp_sec / 60 / 60, (timestamp_sec / 60) % 60, timestamp_sec % 60,
				  timestamp % 1000000,
				  with_timezone ? "+00" : "",
				  (year <= 0) ? " BC" : "");

	return 0;
}

static int
decode_timestamp(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	return decode_timestamp_internal(buffer, buff_size, out_size, false);
}

static int
decode_timestamptz(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	return decode_timestamp_internal(buffer, buff_size, out_size, true);
}

/* Decode a float4 type */
static int
decode_float4(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) INTALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(float))
		return -2;

	CopyAppendFmt("%.12f", *(float *) buffer);
	*out_size = sizeof(float) + delta;
	return 0;
}

/* Decode a float8 type */
static int
decode_float8(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) DOUBLEALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(double))
		return -2;

	CopyAppendFmt("%.12lf", *(double *) buffer);
	*out_size = sizeof(double) + delta;
	return 0;
}

/* Decode an uuid type */
static int
decode_uuid(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	unsigned char uuid[16];

	if (buff_size < sizeof(uuid))
		return -1;

	memcpy(uuid, buffer, sizeof(uuid));
	CopyAppendFmt("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
				  uuid[0], uuid[1], uuid[2], uuid[3], uuid[4], uuid[5], uuid[6], uuid[7],
				  uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
		);
	*out_size = sizeof(uuid);
	return 0;
}

/* Decode a macaddr type */
static int
decode_macaddr(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	unsigned char macaddr[6];
	const char *new_buffer = (const char *) INTALIGN(buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(macaddr))
		return -2;

	memcpy(macaddr, buffer, sizeof(macaddr));
	CopyAppendFmt("%02x:%02x:%02x:%02x:%02x:%02x",
				  macaddr[0], macaddr[1], macaddr[2], macaddr[3], macaddr[4], macaddr[5]
		);
	*out_size = sizeof(macaddr) + delta;
	return 0;
}

/* Decode a bool type */
static int
decode_bool(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	if (buff_size < sizeof(bool))
		return -1;

	CopyAppend(*(bool *) buffer ? "t" : "f");
	*out_size = sizeof(bool);
	return 0;
}

/* Decode a name type (used mostly in catalog tables) */
static int
decode_name(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	if (buff_size < NAMEDATALEN)
		return -1;

	CopyAppendEncode(buffer, strnlen(buffer, NAMEDATALEN));
	*out_size = NAMEDATALEN;
	return 0;
}

/*
 * Decode numeric type.
 */
static int
decode_numeric(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
       int result = extract_data(buffer, buff_size, out_size, &CopyAppendNumeric);
       return result;
}

/* Decode a char type */
static int
decode_char(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	if (buff_size < sizeof(char))
		return -2;

	CopyAppendEncode(buffer, 1);
	*out_size = 1;
	return 0;
}

/* Ignore all data left */
static int
decode_ignore(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	*out_size = buff_size;
	return 0;
}

/* Decode char(N), varchar(N), text, json or xml types */
static int
decode_string(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
       int result = extract_data(buffer, buff_size, out_size, &CopyAppendEncode);
       return result;
}

/*
 * Align data, parse varlena header, detoast and decompress.
 * Last parameters responds for actual parsing according to type.
 */
static int
extract_data(const char *buffer, unsigned int buff_size, unsigned int *out_size, int (*parse_value)(const char *, int))
{
	int			padding = 0;
	int			result	= 0;

	/* Skip padding bytes. */
	while (*buffer == 0x00)
	{
		if (buff_size == 0)
			return -1;

		buff_size--;
		buffer++;
		padding++;
	}

	if (VARATT_IS_1B_E(buffer))
	{
		/*
		 * 00000001 1-byte length word, unaligned, TOAST pointer
		 */
		uint32		len = VARSIZE_EXTERNAL(buffer);

		if (len > buff_size)
			return -1;

		if (blockOptions & BLOCK_DECODE_TOAST)
		{
			result = ReadStringFromToast(buffer, buff_size, out_size, parse_value);
		}
		else if (VARATT_IS_EXTERNAL_ONDISK(buffer))
		{
			varatt_external toast_ptr;
			VARATT_EXTERNAL_GET_POINTER(toast_ptr, buffer);
			if (VARATT_EXTERNAL_IS_COMPRESSED(toast_ptr))
			{
#if PG_VERSION_NUM >= 140000
				switch (VARATT_EXTERNAL_GET_COMPRESS_METHOD(toast_ptr))
				{
					case TOAST_PGLZ_COMPRESSION_ID:
#endif
						CopyAppend("(TOASTED,pglz)");
#if PG_VERSION_NUM >= 140000
						break;
					case TOAST_LZ4_COMPRESSION_ID:
						CopyAppend("(TOASTED,lz4)");
						break;
					default:
						CopyAppend("(TOASTED,unknown)");
						break;
				}
#endif
			}
			else
				CopyAppend("(TOASTED,uncompressed)");
		}
		/* If tag is indirect or expanded, it was stored in memory. */
		else
			CopyAppend("(TOASTED IN MEMORY)");

		*out_size = padding + len;
		return result;
	}

	if (VARATT_IS_1B(buffer))
	{
		/*
		 * xxxxxxx1 1-byte length word, unaligned, uncompressed data (up to
		 * 126b) xxxxxxx is 1 + string length
		 */
		uint8		len = VARSIZE_1B(buffer);

		if (len > buff_size)
			return -1;

		result = parse_value(buffer + 1, len - 1);
		*out_size = padding + len;
		return result;
	}

	if (VARATT_IS_4B_U(buffer) && buff_size >= 4)
	{
		/*
		 * xxxxxx00 4-byte length word, aligned, uncompressed data (up to 1G)
		 */
		uint32		len = VARSIZE_4B(buffer);

		if (len > buff_size)
			return -1;

		result = parse_value(buffer + 4, len - 4);
		*out_size = padding + len;
		return result;
	}

	if (VARATT_IS_4B_C(buffer) && buff_size >= 8)
	{
		/*
		 * xxxxxx10 4-byte length word, aligned, *compressed* data (up to 1G)
		 */
		int						decompress_ret;
		uint32					len = VARSIZE_4B(buffer);
		uint32					decompressed_len = 0;
		char				   *decompress_tmp_buff;
#if PG_VERSION_NUM >= 140000
		ToastCompressionId		cmid;
#endif

#if PG_VERSION_NUM >= 140000
		decompressed_len = VARDATA_COMPRESSED_GET_EXTSIZE(buffer);
#else
		decompressed_len = VARRAWSIZE_4B_C(buffer);
#endif

		if (len > buff_size)
			return -1;

		if ((decompress_tmp_buff = malloc(decompressed_len)) == NULL)
		{
			perror("malloc");
			exit(1);
		}

#if PG_VERSION_NUM >= 140000
		cmid = VARDATA_COMPRESSED_GET_COMPRESS_METHOD(buffer);
		switch(cmid)
		{
			case TOAST_PGLZ_COMPRESSION_ID:
				decompress_ret = pglz_decompress(VARDATA_4B_C(buffer), len - 2 * sizeof(uint32),
												 decompress_tmp_buff, decompressed_len, true);
				break;
#ifdef USE_LZ4
			case TOAST_LZ4_COMPRESSION_ID:
				decompress_ret = LZ4_decompress_safe(VARDATA_4B_C(buffer), decompress_tmp_buff,
													 len - 2 * sizeof(uint32), decompressed_len);
				break;
#endif
			default:
				decompress_ret = -1;
				break;
		}
#else /* PG_VERSION_NUM < 140000 */
		decompress_ret = pglz_decompress(VARDATA_4B_C(buffer), len - 2 * sizeof(uint32),
										 decompress_tmp_buff, decompressed_len
#if PG_VERSION_NUM >= 120000
										 , true
#endif
										 );
#endif /* PG_VERSION_NUM >= 140000 */

		if ((decompress_ret != decompressed_len) || (decompress_ret < 0))
		{
			printf("WARNING: Corrupted toast data, unable to decompress.\n");
			CopyAppend("(inline compressed, corrupted)");
			*out_size = padding + len;
			free(decompress_tmp_buff);
			return 0;
		}

		result = parse_value(decompress_tmp_buff, decompressed_len);
		*out_size = padding + len;
		free(decompress_tmp_buff);
		return result;
	}

	return -9;
}

/*
 * Try to decode a tuple using a types string provided previously.
 *
 * Arguments:
 *   tupleData   - pointer to the tuple data
 *   tupleSize   - tuple size in bytes
 */
void
FormatDecode(const char *tupleData, unsigned int tupleSize)
{
	HeapTupleHeader header = (HeapTupleHeader) tupleData;
	const char *data = tupleData + header->t_hoff;
	unsigned int size = tupleSize - header->t_hoff;
	int			curr_attr;

	CopyClear();

	for (curr_attr = 0; curr_attr < ncallbacks; curr_attr++)
	{
		int			ret;
		unsigned int processed_size = 0;

		if ((header->t_infomask & HEAP_HASNULL) && att_isnull(curr_attr, header->t_bits))
		{
			CopyAppend("\\N");
			continue;
		}

		if (size <= 0)
		{
			printf("Error: unable to decode a tuple, no more bytes left. Partial data: %s\n",
				   copyString.data);
			return;
		}

		ret = callbacks[curr_attr] (data, size, &processed_size);
		if (ret < 0)
		{
			printf("Error: unable to decode a tuple, callback #%d returned %d. Partial data: %s\n",
				   curr_attr + 1, ret, copyString.data);
			return;
		}

		size -= processed_size;
		data += processed_size;
	}

	if (size != 0)
	{
		printf("Error: unable to decode a tuple, %d bytes left, 0 expected. Partial data: %s\n",
			   size, copyString.data);
		return;
	}

	CopyFlush();
}

static int DumpCompressedString(const char *data, int32 compressed_size, int (*parse_value)(const char *, int))
{
	int						decompress_ret;
	char				   *decompress_tmp_buff = malloc(TOAST_COMPRESS_RAWSIZE(data));
	ToastCompressionId		cmid;

	cmid = TOAST_COMPRESS_RAWMETHOD(data);
	switch(cmid)
	{
		case TOAST_PGLZ_COMPRESSION_ID:
			decompress_ret = pglz_decompress(TOAST_COMPRESS_RAWDATA(data),
											 compressed_size - TOAST_COMPRESS_HEADER_SIZE,
											 decompress_tmp_buff, TOAST_COMPRESS_RAWSIZE(data)
#if PG_VERSION_NUM >= 120000
											 , true
#endif
											 );
			break;
		case TOAST_LZ4_COMPRESSION_ID:
#ifdef USE_LZ4
			decompress_ret = LZ4_decompress_safe(TOAST_COMPRESS_RAWDATA(data),
												 decompress_tmp_buff, compressed_size - TOAST_COMPRESS_HEADER_SIZE,
												 TOAST_COMPRESS_RAWSIZE(data));
			break;
#else
			printf("Error: compression method lz4 not supported.\n");
			printf("Try to rebuild pg_filedump for PostgreSQL server of version 14+ with --with-lz4 option.\n");
			free(decompress_tmp_buff);
			return -2;
#endif
		default:
			decompress_ret = -1;
			break;
	}

	if ((decompress_ret != TOAST_COMPRESS_RAWSIZE(data)) ||
			(decompress_ret < 0))
	{
		printf("WARNING: Unable to decompress a string. Data is corrupted.\n");
		printf("Returned %d while expected %d.\n", decompress_ret,
				TOAST_COMPRESS_RAWSIZE(data));
	}
	else
	{
		CopyAppendEncode(decompress_tmp_buff, decompress_ret);
	}

	free(decompress_tmp_buff);

	return decompress_ret;
}

static int
ReadStringFromToast(const char *buffer,
		unsigned int buff_size,
		unsigned int* out_size,
		int (*parse_value)(const char *, int))
{
	int		result = 0;

	/* If toasted value is on disk, we'll try to restore it. */
	if (VARATT_IS_EXTERNAL_ONDISK(buffer))
	{
		varatt_external toast_ptr;
		char	   *toast_data = NULL;
		/* Number of chunks the TOAST data is divided into */
		int32		num_chunks;
		/* Actual size of external TOASTed value */
		int32		toast_ext_size;
		/* Path to directory with TOAST realtion file */
		char	   *toast_relation_path;
		/* Filename of TOAST relation file */
		char		toast_relation_filename[MAXPGPATH];
		FILE	   *toast_rel_fp;
		unsigned int block_options = 0;
		unsigned int control_options = 0;

		VARATT_EXTERNAL_GET_POINTER(toast_ptr, buffer);

		/* Extract TOASTed value */
#if PG_VERSION_NUM >= 140000
		toast_ext_size = VARATT_EXTERNAL_GET_EXTSIZE(toast_ptr);
#else
		toast_ext_size = toast_ptr.va_extsize;
#endif
		num_chunks = (toast_ext_size - 1) / TOAST_MAX_CHUNK_SIZE + 1;

		printf("  TOAST value. Raw size: %8d, external size: %8d, "
				"value id: %6d, toast relation id: %6d, chunks: %6d\n",
				toast_ptr.va_rawsize,
				toast_ext_size,
				toast_ptr.va_valueid,
				toast_ptr.va_toastrelid,
				num_chunks);

		/* Open TOAST relation file */
		toast_relation_path = strdup(fileName);
		get_parent_directory(toast_relation_path);
		sprintf(toast_relation_filename, "%s/%d",
				*toast_relation_path ? toast_relation_path : ".",
				toast_ptr.va_toastrelid);
		toast_rel_fp = fopen(toast_relation_filename, "rb");
		if (!toast_rel_fp) {
			printf("Cannot open TOAST relation %s\n",
					toast_relation_filename);
			result = -1;
		}
		else
		{
			unsigned int toast_relation_block_size = GetBlockSize(toast_rel_fp);
			fseek(toast_rel_fp, 0, SEEK_SET);
			toast_data = malloc(toast_ptr.va_rawsize);

			result = DumpFileContents(block_options,
					control_options,
					toast_rel_fp,
					toast_relation_block_size,
					-1, /* no start block */
					-1, /* no end block */
					true, /* is toast relation */
					toast_ptr.va_valueid,
					toast_ext_size,
					toast_data);

			if (result == 0)
			{
				if (VARATT_EXTERNAL_IS_COMPRESSED(toast_ptr))
					result = DumpCompressedString(toast_data, toast_ext_size, parse_value);
				else
					result = parse_value(toast_data, toast_ext_size);
			}
			else
			{
				printf("Error in TOAST file.\n");
			}

			free(toast_data);
			fclose(toast_rel_fp);
		}

		free(toast_relation_path);
	}
	/* If tag is indirect or expanded, it was stored in memory. */
	else
	{
		CopyAppend("(TOASTED IN MEMORY)");
	}

	return result;
}

/* Decode an Oid as int type and pass value out. */
static int
DecodeOidBinary(const char *buffer,
		unsigned int buff_size,
		unsigned int *processed_size,
		Oid *result)
{
	const char	   *new_buffer = (const char *) INTALIGN(buffer);
	unsigned int	delta =
		(unsigned int)((uintptr_t)new_buffer - (uintptr_t)buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int32))
		return -2;

	*result = *(Oid *)buffer;
	*processed_size = sizeof(Oid) + delta;

	return 0;
}

/* Decode char(N), varchar(N), text, json or xml types and pass data out. */
static int
DecodeBytesBinary(const char *buffer,
		unsigned int buff_size,
		unsigned int *processed_size,
		char *out_data,
		unsigned int *out_length)
{
	if (!VARATT_IS_EXTENDED(buffer))
	{
		*out_length = VARSIZE(buffer) - VARHDRSZ;

		*processed_size = VARSIZE(buffer);
		memcpy(out_data, VARDATA(buffer), *out_length);
	}
	else
	{
		printf("Error: unable read TOAST value.\n");
	}

	return 0;
}

/*
 * Decode a TOAST chunk as a tuple (Oid toast_id, Oid chunk_id, text data).
 * If decoded OID is equal toast_oid, copy data into chunk_data.
 *
 * Parameters:
 *     tuple_data - data of the tuple
 *     tuple_size - length of the tuple
 *     toast_oid - [out] oid of the TOAST value
 *     chunk_id - [out] number of the TOAST chunk stored in the tuple
 *     chunk - [out] extracted chunk data
 *     chunk_size - [out] number of bytes extracted from the chunk
 */
void
ToastChunkDecode(const char *tuple_data,
		unsigned int tuple_size,
		Oid toast_oid,
		uint32 *chunk_id,
		char *chunk_data,
		unsigned int *chunk_data_size)
{
	HeapTupleHeader		header = (HeapTupleHeader)tuple_data;
	const char	   *data = tuple_data + header->t_hoff;
	unsigned int	size = tuple_size - header->t_hoff;
	unsigned int	processed_size = 0;
	Oid				read_toast_oid;
	int				ret;

	*chunk_data_size = 0;
	*chunk_id = 0;

	/* decode toast_id */
	ret = DecodeOidBinary(data, size, &processed_size, &read_toast_oid);
	if (ret < 0)
	{
		printf("Error: unable to decode a TOAST tuple toast_id, "
				"decode function returned %d. Partial data: %s\n",
				ret, copyString.data);
		return;
	}

	size -= processed_size;
	data += processed_size;
	if (size <= 0)
	{
		printf("Error: unable to decode a TOAST chunk tuple, no more bytes "
			   "left. Partial data: %s\n", copyString.data);
		return;
	}

	/* It is not what we are looking for */
	if (toast_oid != read_toast_oid)
		return;

	/* decode chunk_id */
	ret = DecodeOidBinary(data, size, &processed_size, chunk_id);
	if (ret < 0)
	{
		printf("Error: unable to decode a TOAST tuple chunk_id, decode "
				"function returned %d. Partial data: %s\n",
				ret, copyString.data);
		return;
	}

	size -= processed_size;
	data += processed_size;
	if (size <= 0)
	{
		printf("Error: unable to decode a TOAST chunk tuple, no more bytes "
				"left. Partial data: %s\n", copyString.data);
		return;
	}

	/* decode data */
	ret = DecodeBytesBinary(data, size, &processed_size, chunk_data,
			chunk_data_size);
	if (ret < 0)
	{
		printf("Error: unable to decode a TOAST chunk data, decode function "
				"returned %d. Partial data: %s\n", ret, copyString.data);
		return;
	}

	size -= processed_size;
	if (size != 0)
	{
		printf("Error: unable to decode a TOAST chunk tuple, %d bytes left. "
				"Partial data: %s\n", size, copyString.data);
		return;
	}
}
