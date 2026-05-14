/*--------------------------------------------------------------------
 * Symbols referenced in this file:
 * - raw_parser
 * - base_yylex
 * - raw_parser
 *--------------------------------------------------------------------
 */

/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for PostgreSQL grammar
 *
 * Note that the grammar is not allowed to perform any table access
 * (since we need to be able to do basic parsing even while inside an
 * aborted transaction).  Therefore, the data structures returned by
 * the grammar are "raw" parsetrees that still need to be analyzed by
 * analyze.c and related files.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development PGGroup
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/parser/parser.c
 *
 *-------------------------------------------------------------------------
 */

#include "pg_functions.hpp"

#include "parser/gramparse.hpp"
#include "parser/parser.hpp"
#include "parser/kwlist.hpp"

namespace duckdb_libpgquery {

/*
 * raw_parser
 *		Given a query in string form, do lexical and grammatical analysis.
 *
 * Returns a list of raw (un-analyzed) parse trees.  The immediate elements
 * of the list are always PGRawStmt nodes.
 */
PGList *raw_parser(const char *str) {
	core_yyscan_t yyscanner;
	base_yy_extra_type yyextra;
	int yyresult;

	/* initialize the flex scanner */
	yyscanner = scanner_init(str, &yyextra.core_yy_extra, ScanKeywords, NumScanKeywords);

	/* base_yylex() only needs this much initialization */
	yyextra.num_lookahead = 0;

	/* initialize the bison parser */
	parser_init(&yyextra);

	/* Parse! */
	yyresult = base_yyparse(yyscanner);

	/* Clean up (release memory) */
	scanner_finish(yyscanner);

	if (yyresult) /* error */
		return NIL;

	return yyextra.parsetree;
}

PGKeywordCategory is_keyword(const char *text) {
	auto keyword = ScanKeywordLookup(text, ScanKeywords, NumScanKeywords);
	if (keyword) {
		return static_cast<PGKeywordCategory>(keyword->category);
	}
	return PGKeywordCategory::PG_KEYWORD_NONE;
}

std::vector<PGKeyword> keyword_list() {
    std::vector<PGKeyword> result;
	for(size_t i = 0; i < NumScanKeywords; i++) {
		PGKeyword keyword;
		keyword.text = ScanKeywords[i].name;
		switch(ScanKeywords[i].category) {
		case UNRESERVED_KEYWORD:
			keyword.category = PGKeywordCategory::PG_KEYWORD_UNRESERVED;
			break;
		case RESERVED_KEYWORD:
			keyword.category = PGKeywordCategory::PG_KEYWORD_RESERVED;
			break;
		case TYPE_FUNC_NAME_KEYWORD:
			keyword.category = PGKeywordCategory::PG_KEYWORD_TYPE_FUNC;
			break;
		case COL_NAME_KEYWORD:
			keyword.category = PGKeywordCategory::PG_KEYWORD_COL_NAME;
			break;
		}
		result.push_back(keyword);
	}
	return result;
}

std::vector<PGSimplifiedToken> tokenize(const char *str) {
	core_yyscan_t yyscanner;
	base_yy_extra_type yyextra;

	std::vector<PGSimplifiedToken> result;
	yyscanner = scanner_init(str, &yyextra.core_yy_extra, ScanKeywords, NumScanKeywords);
	yyextra.num_lookahead = 0;

	while(true) {
		YYSTYPE type;
		YYLTYPE loc;
		int token;
		try {
			token = base_yylex(&type, &loc, yyscanner);
		} catch(...) {
			token = 0;
		}
		if (token == 0) {
			break;
		}
		PGSimplifiedToken current_token;
		switch(token) {
		case IDENT:
			current_token.type = PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_IDENTIFIER;
			break;
		case ICONST:
		case FCONST:
			current_token.type = PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_NUMERIC_CONSTANT;
			break;
		case SCONST:
		case BCONST:
		case XCONST:
			current_token.type = PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_STRING_CONSTANT;
			break;
		case Op:
		case PARAM:
		case COLON_EQUALS:
		case EQUALS_GREATER:
		case LESS_EQUALS:
		case GREATER_EQUALS:
		case NOT_EQUALS:
			current_token.type = PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_OPERATOR;
			break;
		default:
			if (token >= 255) {
				// non-ascii value, probably a keyword
				current_token.type = PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_KEYWORD;
			} else {
				// ascii value, probably an operator
				current_token.type = PGSimplifiedTokenType::PG_SIMPLIFIED_TOKEN_OPERATOR;
			}
			break;
		}
		current_token.start = loc;
		result.push_back(current_token);
	}

	scanner_finish(yyscanner);
	return result;
}



/*
 * peek_token --- ensure at least depth+1 tokens are buffered after the
 * currently-active one, then return the token kind at that position.
 *
 * Caller must have set yyextra->cur_end to the byte in scanbuf where the
 * active token ends (the byte the scanner has injected '\0' into).  Each
 * peek lazily calls core_yylex; core_yylex restores the previous boundary's
 * original character and injects '\0' at the end of the newly-scanned token.
 * We then re-inject '\0' at the previous boundary (saving the restored char
 * into hold_char) so error reporting for earlier tokens still works.
 */
static int peek_token(base_yy_extra_type *yyextra,
                      core_yyscan_t yyscanner,
                      int depth) {
	while (yyextra->num_lookahead <= depth) {
		core_YYSTYPE yylval;
		YYLTYPE yylloc;
		int token = core_yylex(&yylval, &yylloc, yyscanner);

		/*
		 * core_yylex restored the '\0' at the previous boundary to its
		 * original char.  Capture it and re-inject '\0' for error reporting.
		 */
		char *prev_end;
		if (yyextra->num_lookahead == 0) {
			prev_end = yyextra->cur_end;
			yyextra->cur_hold_char = *prev_end;
		} else {
			int last = yyextra->num_lookahead - 1;
			prev_end = yyextra->lookahead[last].end;
			yyextra->lookahead[last].hold_char = *prev_end;
		}
		*prev_end = '\0';

		/* Locate end of newly-peeked token: scan to the scanner's '\0'. */
		char *tok_end = yyextra->core_yy_extra.scanbuf + yylloc;
		while (*tok_end != '\0') {
			++tok_end;
		}

		int idx = yyextra->num_lookahead++;
		yyextra->lookahead[idx].token = token;
		yyextra->lookahead[idx].yylval = yylval;
		yyextra->lookahead[idx].yylloc = yylloc;
		yyextra->lookahead[idx].end = tok_end;
		yyextra->lookahead[idx].hold_char = '\0';  /* placeholder; set when next slot is peeked */
	}
	return yyextra->lookahead[depth].token;
}

/*
 * Intermediate filter between parser and core lexer (core_yylex in scan.l).
 *
 * This filter is needed because in some cases the standard SQL grammar
 * requires more than one token lookahead.  We reduce these cases to
 * single-token productions by replacing tokens here, keeping the grammar
 * LALR(1).
 *
 * Using a filter is simpler than trying to recognize multiword tokens
 * directly in scan.l, because we'd have to allow for comments between the
 * words.  Furthermore it's not clear how to do that without re-introducing
 * scanner backtrack, which would cost more performance than this filter
 * layer does.
 *
 * The filter also provides a convenient place to translate between
 * the core_YYSTYPE and YYSTYPE representations (which are really the
 * same thing anyway, but notationally they're different).
 */
int base_yylex(YYSTYPE *lvalp, YYLTYPE *llocp, core_yyscan_t yyscanner) {
	base_yy_extra_type *yyextra = pg_yyget_extra(yyscanner);
	int cur_token;

	/* Get current token: pop buffered slot, or scan fresh. */
	if (yyextra->num_lookahead > 0) {
		/* Restore the byte at end of OLD current to its original char. */
		*(yyextra->cur_end) = yyextra->cur_hold_char;
		cur_token = yyextra->lookahead[0].token;
		lvalp->core_yystype = yyextra->lookahead[0].yylval;
		*llocp = yyextra->lookahead[0].yylloc;
		yyextra->cur_end = yyextra->lookahead[0].end;
		yyextra->cur_hold_char = yyextra->lookahead[0].hold_char;
		for (int i = 0; i + 1 < yyextra->num_lookahead; ++i) {
			yyextra->lookahead[i] = yyextra->lookahead[i + 1];
		}
		yyextra->num_lookahead--;
	} else {
		cur_token = core_yylex(&(lvalp->core_yystype), llocp, yyscanner);
	}

	/*
	 * Tokens that may need lookahead-based reclassification.  We need the
	 * length only to initialize cur_end the first time we peek; once
	 * cur_end is set (either by a previous peek, or below for fresh tokens),
	 * subsequent peeks operate against the already-known boundary.
	 */
	int cur_token_length;
	switch (cur_token) {
	case NOT:
		cur_token_length = 3;
		break;
	case NULLS_P:
		cur_token_length = 5;
		break;
	case WITH:
		cur_token_length = 4;
		break;
	case AT:
		cur_token_length = 2;
		break;
	default:
		return cur_token;
	}

	/*
	 * If we got cur_token from the lookahead buffer, cur_end was already
	 * set when this slot was first peeked.  Otherwise initialize it now;
	 * the scanner's '\0' should currently terminate cur_token.
	 */
	if (yyextra->num_lookahead == 0) {
		yyextra->cur_end = yyextra->core_yy_extra.scanbuf + *llocp + cur_token_length;
		Assert(*(yyextra->cur_end) == '\0');
	}

	/* Make filter decisions, peeking as needed. */
	switch (cur_token) {
	case NOT:
		/* Replace NOT by NOT_LA if it's followed by BETWEEN, IN, etc */
		switch (peek_token(yyextra, yyscanner, 0)) {
		case BETWEEN:
		case IN_P:
		case LIKE:
		case ILIKE:
		case SIMILAR:
			cur_token = NOT_LA;
			break;
		}
		break;

	case NULLS_P:
		/* Replace NULLS_P by NULLS_LA if it's followed by FIRST or LAST */
		switch (peek_token(yyextra, yyscanner, 0)) {
		case FIRST_P:
		case LAST_P:
			cur_token = NULLS_LA;
			break;
		}
		break;

	case WITH:
		/* Replace WITH by WITH_LA if it's followed by TIME or ORDINALITY */
		switch (peek_token(yyextra, yyscanner, 0)) {
		case TIME:
		case ORDINALITY:
			cur_token = WITH_LA;
			break;
		}
		break;

	case AT: {
		/*
		 * Replace AT by AT_LA only for the two specific forms that the
		 * grammar treats as AT-prefixed productions:
		 *   AT TIME ZONE expr                              (timezone conversion)
		 *   AT '(' (VERSION|TIMESTAMP) '=>' expr ')'       (time-travel)
		 * Anywhere else, leave AT as a plain unreserved keyword so it can
		 * be used as a column name, table alias, etc.  The 3-token peek
		 * for the time-travel form distinguishes it from the column-rename
		 * alias form `tbl AS at (col, ...)`.
		 */
		int la1 = peek_token(yyextra, yyscanner, 0);
		if (la1 == TIME) {
			cur_token = AT_LA;
		} else if (la1 == '(') {
			int la2 = peek_token(yyextra, yyscanner, 1);
			if (la2 == VERSION_P || la2 == TIMESTAMP) {
				if (peek_token(yyextra, yyscanner, 2) == EQUALS_GREATER) {
					cur_token = AT_LA;
				}
			}
		}
		break;
	}
	}

	return cur_token;
}

}
