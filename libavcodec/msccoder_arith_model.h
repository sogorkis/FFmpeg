/*
 * Arithmetic coder. Based on Mark Nelson article: "Arithmetic Coding
 * + Statistical Modeling = Data Compression".
 *
 * Listing 8 -- model.h
 *
 * This file contains all of the function prototypes and
 * external variable declarations needed to interface with
 * the modeling code found in model-1.c or model-2.c.
 */

#ifndef MSCCODER_ARITH_MODEL_H
#define MSCCODER_ARITH_MODEL_H

#include "msccoder_arith.h"

/*
 * Eternal variable declarations.
 */
extern int max_order;
extern int flushing_enabled;

typedef struct MscCoderArithModel {
	/*
	 * In order to create an array with indices -1 through 256, I have
	 * to do this funny declaration.  totals[-1] == storage[0].
	 */
	short int *storage;
	short int *totals;
	int length;
} MscCoderArithModel;

/*
 * Prototypes for routines that can be called from MODEL-X.C
 */
void initialize_model( MscCoderArithModel * m, int bits );
void update_model( MscCoderArithModel * m, int symbol );
int convert_int_to_symbol( MscCoderArithModel * m, int symbol, MscCoderArithSymbol *s );
void get_symbol_scale( MscCoderArithModel * m, MscCoderArithSymbol *s );
int convert_symbol_to_int( MscCoderArithModel * m, int count, MscCoderArithSymbol *s );
void free_model(MscCoderArithModel * m);
void add_character_to_model( int c );
void flush_model( void );

#endif
