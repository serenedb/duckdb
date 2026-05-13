/*****************************************************************************
 *
 *		QUERY: CREATE INDEX
 *
 * Note: we cannot put TABLESPACE clause after WHERE clause unless we are
 * willing to make TABLESPACE a fully reserved word.
 *****************************************************************************/
IndexStmt:	CREATE_P opt_unique INDEX opt_concurrently opt_index_name
			ON qualified_name access_method_clause '(' index_params ')'
			opt_include opt_reloptions where_clause
				{
					PGIndexStmt *n = makeNode(PGIndexStmt);
					n->unique = $2;
					n->concurrent = $4;
					n->idxname = $5;
					n->relation = $7;
					n->accessMethod = $8;
					n->indexParams = list_concat($10, $12);
					n->options = $13;
					n->whereClause = $14;
					n->excludeOpNames = NIL;
					n->idxcomment = NULL;
					n->indexOid = InvalidOid;
					n->oldNode = InvalidOid;
					n->primary = false;
					n->isconstraint = false;
					n->deferrable = false;
					n->initdeferred = false;
					n->transformed = false;
					n->onconflict = PG_ERROR_ON_CONFLICT;
					$$ = (PGNode *)n;
				}
			| CREATE_P opt_unique INDEX opt_concurrently IF_P NOT EXISTS index_name
			ON qualified_name access_method_clause '(' index_params ')'
			opt_include opt_reloptions where_clause
				{
					PGIndexStmt *n = makeNode(PGIndexStmt);
					n->unique = $2;
					n->concurrent = $4;
					n->idxname = $8;
					n->relation = $10;
					n->accessMethod = $11;
					n->indexParams = list_concat($13, $15);
					n->options = $16;
					n->whereClause = $17;
					n->excludeOpNames = NIL;
					n->idxcomment = NULL;
					n->indexOid = InvalidOid;
					n->oldNode = InvalidOid;
					n->primary = false;
					n->isconstraint = false;
					n->deferrable = false;
					n->initdeferred = false;
					n->transformed = false;
					n->onconflict = PG_IGNORE_ON_CONFLICT;
					$$ = (PGNode *)n;
				}
		;


/*
 * INCLUDE clause: appends the listed columns to indexParams marked with
 * opclass "included". The catalog-side handler reads that as a "store the
 * value, don't tokenize/index for filtering" signal -- routes the column
 * into the typed columnstore.
 */
opt_include:
			INCLUDE_P '(' index_including_params ')'		{ $$ = $3; }
			| /* EMPTY */									{ $$ = NIL; }
		;

index_including_params:
			index_elem
				{
					PGIndexElem *e = $1;
					if (e->opclass == NULL) {
						e->opclass = list_make1(makeString("included"));
					}
					$$ = list_make1(e);
				}
			| index_including_params ',' index_elem
				{
					PGIndexElem *e = $3;
					if (e->opclass == NULL) {
						e->opclass = list_make1(makeString("included"));
					}
					$$ = lappend($1, e);
				}
		;


access_method:
			ColId									{ $$ = $1; };


access_method_clause:
			USING access_method						{ $$ = $2; }
			| /*EMPTY*/								{ $$ = (char*) DEFAULT_INDEX_TYPE; }
		;


opt_concurrently:
			CONCURRENTLY							{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;


opt_index_name:
			index_name								{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NULL; }
		;


opt_reloptions:		WITH reloptions					{ $$ = $2; }
			 |		/* EMPTY */						{ $$ = NIL; }
		;


opt_unique:
			UNIQUE									{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;
