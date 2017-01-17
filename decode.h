#ifndef _PG_FILEDUMP_DECODE_H_
#define _PG_FILEDUMP_DECODE_H_

int
ParseAttributeTypesString(const char* str);

void
FormatDecode(const char* tupleData, unsigned int tupleSize);

#endif
