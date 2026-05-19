/*-------------------------------------------------------------------------
 *
 * gramparse.h
 *		Shared definitions for the "raw" parser (flex and bison phases only)
 *
 * NOTE: this file is only meant to be included in the core parsing files,
 * ie, parser.c, gram.y, scan.l, and src/common/keywords.c.
 * Definitions that are needed outside the core parser should be in parser.h.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development PGGroup
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/parser/gramparse.h
 *
 *-------------------------------------------------------------------------
 */

#pragma once

#include "nodes/parsenodes.hpp"
#include "parser/scanner.hpp"

namespace duckdb_libpgquery {
#include "parser/gram.hpp"

/*
 * One slot of buffered lookahead.  base_yylex peeks 1-3 tokens ahead to
 * disambiguate certain keywords (NOT, NULLS_P, WITH, AT); each peeked token
 * is captured here so subsequent base_yylex calls can return it without
 * re-scanning.
 */
typedef struct base_yy_lookahead_slot {
	int token;
	core_YYSTYPE yylval;
	YYLTYPE yylloc;
	char *end;       /* address in scanbuf where this token ends */
	char hold_char;  /* original char at *end; valid once the next slot is peeked */
} base_yy_lookahead_slot;

/*
 * The YY_EXTRA data that a flex scanner allows us to pass around.  Private
 * state needed for raw parsing/lexing goes here.
 */
typedef struct base_yy_extra_type {
	/*
	 * Fields used by the core scanner.
	 */
	core_yy_extra_type core_yy_extra;

	/*
	 * State variables for base_yylex().  Up to three tokens may be buffered
	 * beyond the currently-active one; slots are consumed FIFO (lookahead[0]
	 * is returned next).  cur_end is the byte in scanbuf where the active
	 * token ends; while num_lookahead > 0 that byte holds '\0' (for error
	 * reporting), with the original character stashed in cur_hold_char.
	 */
	int num_lookahead;
	base_yy_lookahead_slot lookahead[3];
	char *cur_end;
	char cur_hold_char;

	/*
	 * State variables that belong to the grammar.
	 */
	PGList *parsetree; /* final parse result is delivered here */
} base_yy_extra_type;

/*
 * In principle we should use yyget_extra() to fetch the yyextra field
 * from a yyscanner struct.  However, flex always puts that field first,
 * and this is sufficiently performance-critical to make it seem worth
 * cheating a bit to use an inline macro.
 */
#define pg_yyget_extra(yyscanner) (*((base_yy_extra_type **)(yyscanner)))

/* from parser.c */
int base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner);

/* from gram.y */
void parser_init(base_yy_extra_type *yyext);
int base_yyparse(core_yyscan_t yyscanner);

}
