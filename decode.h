#ifndef _PG_FILEDUMP_DECODE_H_
#define _PG_FILEDUMP_DECODE_H_

#define NBASE          10000
#define HALF_NBASE     5000
#define DEC_DIGITS     4                       /* decimal digits per NBASE digit */
#define MUL_GUARD_DIGITS       2       /* these are measured in NBASE digits */
#define DIV_GUARD_DIGITS       4

typedef int16 NumericDigit;

int
ParseAttributeTypesString(const char *str);

void
FormatDecode(const char *tupleData, unsigned int tupleSize);

void
ToastChunkDecode(const char* tuple_data,
		unsigned int tuple_size,
		Oid toast_oid,
		Oid *read_toast_oid,
		uint32 *chunk_id,
		uint32 *want_chunk_id,
		char *chunk_data,
		unsigned int *chunk_data_size);

struct NumericShort
{
       uint16          n_header;               /* Sign + display scale + weight */
       NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Digits */
};

struct NumericLong
{
       uint16          n_sign_dscale;  /* Sign + display scale */
       int16           n_weight;               /* Weight of 1st digit  */
       NumericDigit n_data[FLEXIBLE_ARRAY_MEMBER]; /* Digits */
};

union NumericChoice
{
       uint16          n_header;               /* Header word */
       struct NumericLong n_long;      /* Long form (4-byte header) */
       struct NumericShort n_short;    /* Short form (2-byte header) */
};

struct NumericData
{
       union NumericChoice choice; /* choice of format */
};

/*
 * Interpretation of high bits.
 */

#define NUMERIC_SIGN_MASK      0xC000
#define NUMERIC_POS                    0x0000
#define NUMERIC_NEG                    0x4000
#define NUMERIC_SHORT          0x8000
#define NUMERIC_SPECIAL                0xC000

#define NUMERIC_FLAGBITS(n) ((n)->choice.n_header & NUMERIC_SIGN_MASK)
#define NUMERIC_IS_SHORT(n)            (NUMERIC_FLAGBITS(n) == NUMERIC_SHORT)
#define NUMERIC_IS_SPECIAL(n)  (NUMERIC_FLAGBITS(n) == NUMERIC_SPECIAL)

#define NUMERIC_HDRSZ  (VARHDRSZ + sizeof(uint16) + sizeof(int16))
#define NUMERIC_HDRSZ_SHORT (VARHDRSZ + sizeof(uint16))

/*
 * If the flag bits are NUMERIC_SHORT or NUMERIC_SPECIAL, we want the short
 * header; otherwise, we want the long one.  Instead of testing against each
 * value, we can just look at the high bit, for a slight efficiency gain.
 */
#define NUMERIC_HEADER_IS_SHORT(n)     (((n)->choice.n_header & 0x8000) != 0)
#define NUMERIC_HEADER_SIZE(n) \
       (sizeof(uint16) + \
        (NUMERIC_HEADER_IS_SHORT(n) ? 0 : sizeof(int16)))

/*
 * Definitions for special values (NaN, positive infinity, negative infinity).
 *
 * The two bits after the NUMERIC_SPECIAL bits are 00 for NaN, 01 for positive
 * infinity, 11 for negative infinity.  (This makes the sign bit match where
 * it is in a short-format value, though we make no use of that at present.)
 * We could mask off the remaining bits before testing the active bits, but
 * currently those bits must be zeroes, so masking would just add cycles.
 */
#define NUMERIC_EXT_SIGN_MASK  0xF000  /* high bits plus NaN/Inf flag bits */
#define NUMERIC_NAN                            0xC000
#define NUMERIC_PINF                   0xD000
#define NUMERIC_NINF                   0xF000
#define NUMERIC_INF_SIGN_MASK  0x2000

#define NUMERIC_EXT_FLAGBITS(n)        ((n)->choice.n_header & NUMERIC_EXT_SIGN_MASK)
#define NUMERIC_IS_NAN(n)              ((n)->choice.n_header == NUMERIC_NAN)
#define NUMERIC_IS_PINF(n)             ((n)->choice.n_header == NUMERIC_PINF)
#define NUMERIC_IS_NINF(n)             ((n)->choice.n_header == NUMERIC_NINF)
#define NUMERIC_IS_INF(n) \
       (((n)->choice.n_header & ~NUMERIC_INF_SIGN_MASK) == NUMERIC_PINF)

/*
 * Short format definitions.
 */

#define NUMERIC_SHORT_SIGN_MASK                        0x2000
#define NUMERIC_SHORT_DSCALE_MASK              0x1F80
#define NUMERIC_SHORT_DSCALE_SHIFT             7
#define NUMERIC_SHORT_DSCALE_MAX               \
       (NUMERIC_SHORT_DSCALE_MASK >> NUMERIC_SHORT_DSCALE_SHIFT)
#define NUMERIC_SHORT_WEIGHT_SIGN_MASK 0x0040
#define NUMERIC_SHORT_WEIGHT_MASK              0x003F
#define NUMERIC_SHORT_WEIGHT_MAX               NUMERIC_SHORT_WEIGHT_MASK
#define NUMERIC_SHORT_WEIGHT_MIN               (-(NUMERIC_SHORT_WEIGHT_MASK+1))

/*
 * Extract sign, display scale, weight.  These macros extract field values
 * suitable for the NumericVar format from the Numeric (on-disk) format.
 *
 * Note that we don't trouble to ensure that dscale and weight read as zero
 * for an infinity; however, that doesn't matter since we never convert
 * "special" numerics to NumericVar form.  Only the constants defined below
 * (const_nan, etc) ever represent a non-finite value as a NumericVar.
 */

#define NUMERIC_DSCALE_MASK                    0x3FFF
#define NUMERIC_DSCALE_MAX                     NUMERIC_DSCALE_MASK

#define NUMERIC_SIGN(n) \
       (NUMERIC_IS_SHORT(n) ? \
               (((n)->choice.n_short.n_header & NUMERIC_SHORT_SIGN_MASK) ? \
                NUMERIC_NEG : NUMERIC_POS) : \
               (NUMERIC_IS_SPECIAL(n) ? \
                NUMERIC_EXT_FLAGBITS(n) : NUMERIC_FLAGBITS(n)))
#define NUMERIC_DSCALE(n)      (NUMERIC_HEADER_IS_SHORT((n)) ? \
       ((n)->choice.n_short.n_header & NUMERIC_SHORT_DSCALE_MASK) \
               >> NUMERIC_SHORT_DSCALE_SHIFT \
       : ((n)->choice.n_long.n_sign_dscale & NUMERIC_DSCALE_MASK))
#define NUMERIC_WEIGHT(n)      (NUMERIC_HEADER_IS_SHORT((n)) ? \
       (((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_SIGN_MASK ? \
               ~NUMERIC_SHORT_WEIGHT_MASK : 0) \
        | ((n)->choice.n_short.n_header & NUMERIC_SHORT_WEIGHT_MASK)) \
       : ((n)->choice.n_long.n_weight))


#endif
