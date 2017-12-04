#ifndef _PG_FILEDUMP_DECODE_H_
#define _PG_FILEDUMP_DECODE_H_

int
ParseAttributeTypesString(const char *str);

void
FormatDecode(const char *tupleData, unsigned int tupleSize);

void
ToastChunkDecode(const char* tuple_data,
		unsigned int tuple_size,
		Oid toast_oid,
		uint32 *chunk_id,
		char *chunk_data,
		unsigned int *chunk_data_size);

#endif
