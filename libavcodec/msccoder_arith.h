/*
 * Arithmetic coder. Based on Mark Nelson article: "Arithmetic Coding
 * + Statistical Modeling = Data Compression".
 *
 * Listing 1 -- coder.h
 *
 * This header file contains the constants, declarations, and
 * prototypes needed to use the arithmetic coding routines.  These
 * declarations are for routines that need to interface with the
 * arithmetic coding stuff in coder.c
 *
 */

#ifndef MSCCODER_ARITH_H
#define MSCCODER_ARITH_H

#include "put_bits.h"
#include "get_bits.h"

#define MSC_CODER_ARITH_MAXIMUM_SCALE   16383  /* Maximum allowed frequency count */
#define MSC_CODER_ARITH_ESCAPE          256    /* The escape symbol               */
#define MSC_CODER_ARITH_DONE            -1     /* The output stream empty  symbol */
#define MSC_CODER_ARITH_FLUSH           -2     /* The symbol to flush the model   */

/*
 * A symbol can either be represented as an int, or as a pair of
 * counts on a scale.  This structure gives a standard way of
 * defining it as a pair of counts.
 */
typedef struct {
                unsigned short int low_count;
                unsigned short int high_count;
                unsigned short int scale;
               } MscCoderArithSymbol;

extern long underflow_bits;    /* The present underflow count in  */
                               /* the arithmetic coder.           */
/*
 * Function prototypes.
 */
void initialize_arithmetic_decoder( GetBitContext *bitContext );
void remove_symbol_from_stream( GetBitContext *bitContext, MscCoderArithSymbol *s );
void initialize_arithmetic_encoder( void );
void encode_symbol( PutBitContext *bitContext, MscCoderArithSymbol *s );
void flush_arithmetic_encoder( PutBitContext *bitContext );
short int get_current_count( MscCoderArithSymbol *s );

#endif /* MSCCODER_ARITH_H */
