/*****************************************************************************
 *
 *		QUERY:
 *				DELETE STATEMENTS
 *
 *****************************************************************************/
DeleteStmt: opt_with_clause DELETE_P FROM relation_expr_opt_alias
			using_clause where_or_current_clause returning_clause
				{
					PGDeleteStmt *n = makeNode(PGDeleteStmt);
					n->relation = $4;
					n->usingClause = $5;
					n->whereClause = $6;
					n->returningList = $7;
					n->withClause = $1;
					$$ = (PGNode *)n;
				}
			/*
			 *  TRUNCATE [ TABLE ] [ ONLY ] name [ * ] [, ...]
			 *      [ RESTART IDENTITY | CONTINUE IDENTITY ]
			 *      [ CASCADE | RESTRICT ]
			 *
			 * Lives under DeleteStmt because TRUNCATE is conceptually a
			 * bulk DELETE -- this keeps the statement-list registration
			 * shared and the dispatch lands in TransformTruncate via the
			 * PGTruncateStmt node tag.
			 */
			| TRUNCATE opt_table relation_expr_list opt_restart_seqs opt_drop_behavior
				{
					PGTruncateStmt *n = makeNode(PGTruncateStmt);
					n->relations = $3;
					n->restart_seqs = $4;
					n->behavior = $5;
					$$ = (PGNode *)n;
				}
		;


relation_expr_opt_alias: relation_expr					%prec UMINUS
				{
					$$ = $1;
				}
			| relation_expr ColId
				{
					PGAlias *alias = makeNode(PGAlias);
					alias->aliasname = $2;
					$1->alias = alias;
					$$ = $1;
				}
			| relation_expr AS ColId
				{
					PGAlias *alias = makeNode(PGAlias);
					alias->aliasname = $3;
					$1->alias = alias;
					$$ = $1;
				}
		;


where_or_current_clause:
			WHERE a_expr							{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NULL; }
		;



using_clause:
				USING from_list_opt_comma						{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;


/* Helpers for the TRUNCATE branch above. */
relation_expr_list:
			relation_expr							{ $$ = list_make1($1); }
			| relation_expr_list ',' relation_expr	{ $$ = lappend($1, $3); }
		;


opt_restart_seqs:
			CONTINUE_P IDENTITY_P	{ $$ = false; }
			| RESTART IDENTITY_P	{ $$ = true; }
			| /*EMPTY*/				{ $$ = false; }
		;
