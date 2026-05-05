/*****************************************************************************
 *
 *		QUERY:
 *				TRUNCATE STATEMENT (PostgreSQL syntax)
 *
 *  TRUNCATE [ TABLE ] [ ONLY ] name [ * ] [, ...]
 *      [ RESTART IDENTITY | CONTINUE IDENTITY ] [ CASCADE | RESTRICT ]
 *
 *****************************************************************************/

TruncateStmt:
			TRUNCATE opt_table relation_expr_list opt_restart_seqs opt_drop_behavior
				{
					PGTruncateStmt *n = makeNode(PGTruncateStmt);
					n->relations = $3;
					n->restart_seqs = $4;
					n->behavior = $5;
					$$ = (PGNode *)n;
				}
		;


relation_expr_list:
			relation_expr							{ $$ = list_make1($1); }
			| relation_expr_list ',' relation_expr	{ $$ = lappend($1, $3); }
		;


opt_restart_seqs:
			CONTINUE_P IDENTITY_P	{ $$ = false; }
			| RESTART IDENTITY_P	{ $$ = true; }
			| /*EMPTY*/				{ $$ = false; }
		;
