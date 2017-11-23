#include "postgres.h"
#include "decode.h"
#include <lib/stringinfo.h>
#include <access/htup_details.h>
#include <access/tupmacs.h>
#include <datatype/timestamp.h>
#include <common/pg_lzcompress.h>
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <assert.h>

#define ATTRTYPES_STR_MAX_LEN (1024-1)

typedef int (*decode_callback_t) (const char *buffer, unsigned int buff_size,
								  unsigned int *out_size);

static int
decode_smallint(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_int(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_bigint(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_time(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_timetz(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_date(const char *buffer, unsigned int buff_size, unsigned int *out_size);

static int
decode_timestamp(const char *buffer, unsigned int buff_size, unsigned int *out_size);

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
		"oid", &decode_int
	},
	{
		"xid", &decode_int
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
		"char", &decode_char
	},
	{
		"~", &decode_ignore
	},

	/* internally all string types are stored the same way */
	{
		"charN", &decode_string
	},
	{
		"varchar", &decode_string
	},
	{
		"varcharN", &decode_string
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

/*
 * Temporary buffer for storing decompressed data.
 *
 * 64K should be enough in most cases. If it's not user can manually change
 * this limit. Unfortunately there is no way to know how much memory user
 * is willing to allocate.
 */
static char decompress_tmp_buff[64 * 1024];

/* Used by some PostgreSQL macro definitions */
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
static void
CopyAppendEncode(const char *str, int orig_len)
{
	/*
	 * Should be enough in most cases. If it's not user can manually change
	 * this limit. Unfortunately there is no way to know how much memory user
	 * is willing to allocate.
	 */
	static char tmp_buff[64 * 1024];

	/* Reserve one byte for a trailing zero. */
	const int	max_offset = sizeof(tmp_buff) - 2;
	int			curr_offset = 0;
	int			len = orig_len;

	while (len > 0)
	{
		/*
		 * Make sure there is enough free space for at least one special
		 * symbol and a trailing zero.
		 */
		if (curr_offset > max_offset - 2)
		{
			printf("ERROR: Unable to properly encode a string since it's too "
				   "large (%d bytes). Try to increase tmp_buff size in CopyAppendEncode "
				   "procedure.\n", orig_len);
			exit(1);
		}

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
}

/* CopyAppend version with format string support */
#define CopyAppendFmt(fmt, ...) do { \
	  char __copy_format_buff[512]; \
	  snprintf(__copy_format_buff, sizeof(__copy_format_buff), fmt, ##__VA_ARGS__); \
	  CopyAppend(__copy_format_buff); \
  } while(0)

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
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int16), (uintptr_t) buffer);
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
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int32), (uintptr_t) buffer);
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

/* Decode a bigint type */
static int
decode_bigint(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int64), (uintptr_t) buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < sizeof(int64))
		return -2;

	CopyAppendFmt("%ld", *(int64 *) buffer);
	*out_size = sizeof(int64) + delta;
	return 0;
}

/* Decode a time type */
static int
decode_time(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int64), (uintptr_t) buffer);
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

	CopyAppendFmt("%02ld:%02ld:%02ld.%06ld",
				  timestamp_sec / 60 / 60, (timestamp_sec / 60) % 60, timestamp_sec % 60,
				  timestamp % 1000000);

	return 0;
}

/* Decode a timetz type */
static int
decode_timetz(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int64), (uintptr_t) buffer);
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

	CopyAppendFmt("%02ld:%02ld:%02ld.%06ld%c%02d:%02d",
				  timestamp_sec / 60 / 60, (timestamp_sec / 60) % 60, timestamp_sec % 60,
				  timestamp % 1000000, (tz_min > 0 ? '+' : '-'), abs(tz_min / 60), abs(tz_min % 60));

	return 0;
}

/* Decode a date type */
static int
decode_date(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int32), (uintptr_t) buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);
	int32		jd,
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

	jd = *(int32 *) buffer + POSTGRES_EPOCH_JDATE;
	j2date(jd, &year, &month, &day);

	CopyAppendFmt("%04d-%02d-%02d%s", (year <= 0) ? -year + 1 : year, month, day, (year <= 0) ? " BC" : "");

	return 0;
}

/* Decode a timestamp type */
static int
decode_timestamp(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int64), (uintptr_t) buffer);
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

	CopyAppendFmt("%04d-%02d-%02d %02ld:%02ld:%02ld.%06ld%s",
				  (year <= 0) ? -year + 1 : year, month, day,
				  timestamp_sec / 60 / 60, (timestamp_sec / 60) % 60, timestamp_sec % 60,
				  timestamp % 1000000,
				  (year <= 0) ? " BC" : "");

	return 0;
}

/* Decode a float4 type */
static int
decode_float4(const char *buffer, unsigned int buff_size, unsigned int *out_size)
{
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(float), (uintptr_t) buffer);
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
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(double), (uintptr_t) buffer);
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
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(int32), (uintptr_t) buffer);
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
	const char *new_buffer = (const char *) TYPEALIGN(sizeof(uint32), (uintptr_t) buffer);
	unsigned int delta = (unsigned int) ((uintptr_t) new_buffer - (uintptr_t) buffer);

	if (buff_size < delta)
		return -1;

	buff_size -= delta;
	buffer = new_buffer;

	if (buff_size < NAMEDATALEN)
		return -2;

	CopyAppendEncode(buffer, strnlen(buffer, NAMEDATALEN));
	*out_size = NAMEDATALEN + delta;
	return 0;
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
	int			padding = 0;

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
		uint8		tag = VARTAG_1B_E(buffer);
		uint32		len = VARTAG_SIZE(tag);

		if (len > buff_size)
			return -1;

		CopyAppend("(TOASTED)");
		*out_size = padding + len;
		return 0;
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

		CopyAppendEncode(buffer + 1, len - 1);
		*out_size = padding + len;
		return 0;
	}

	if (VARATT_IS_4B_U(buffer) && buff_size >= 4)
	{
		/*
		 * xxxxxx00 4-byte length word, aligned, uncompressed data (up to 1G)
		 */
		uint32		len = VARSIZE_4B(buffer);

		if (len > buff_size)
			return -1;

		CopyAppendEncode(buffer + 4, len - 4);
		*out_size = padding + len;
		return 0;
	}

	if (VARATT_IS_4B_C(buffer) && buff_size >= 8)
	{
		/*
		 * xxxxxx10 4-byte length word, aligned, *compressed* data (up to 1G)
		 */
		int			decompress_ret;
		uint32		len = VARSIZE_4B(buffer);
		uint32		decompressed_len = VARRAWSIZE_4B_C(buffer);

		if (len > buff_size)
			return -1;

		if (decompressed_len > sizeof(decompress_tmp_buff))
		{
			printf("WARNING: Unable to decompress a string since it's too "
				   "large (%d bytes after decompressing). Consider increasing "
				   "decompress_tmp_buff size.\n", decompressed_len);

			CopyAppend("(COMPRESSED)");
			*out_size = padding + len;
			return 0;
		}

		decompress_ret = pglz_decompress(VARDATA_4B_C(buffer), len - 2 * sizeof(uint32),
										 decompress_tmp_buff, decompressed_len);
		if ((decompress_ret != decompressed_len) || (decompress_ret < 0))
		{
			printf("WARNING: Unable to decompress a string. Data is corrupted.\n");
			CopyAppend("(COMPRESSED)");
			*out_size = padding + len;
			return 0;
		}

		CopyAppendEncode(decompress_tmp_buff, decompressed_len);
		*out_size = padding + len;
		return 0;
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
