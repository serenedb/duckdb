/*****************************************************************************
 *
 *		QUERY:
 *				DEALLOCATE [PREPARE] <plan_name>
 *				DISCARD ALL | PLANS | SEQUENCES | TEMP | TEMPORARY
 *
 * SereneDB has no temp tables / NOTIFY queue / exposed session sequences,
 * so every DISCARD variant collapses to DEALLOCATE ALL semantically.
 * Pooled PG drivers (Npgsql, pgJDBC connection pools, ...) emit DISCARD ALL
 * between checkouts, so this alias keeps real-world driver flows green
 * without requiring a new statement node + executor path.
 *****************************************************************************/
DeallocateStmt: DEALLOCATE name
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = $2;
						$$ = (PGNode *) n;
					}
				| DEALLOCATE PREPARE name
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = $3;
						$$ = (PGNode *) n;
					}
				| DEALLOCATE ALL
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = NULL;
						$$ = (PGNode *) n;
					}
				| DEALLOCATE PREPARE ALL
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = NULL;
						$$ = (PGNode *) n;
					}
				| DISCARD ALL
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = NULL;
						$$ = (PGNode *) n;
					}
				| DISCARD PLANS
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = NULL;
						$$ = (PGNode *) n;
					}
				| DISCARD SEQUENCES
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = NULL;
						$$ = (PGNode *) n;
					}
				| DISCARD TEMP
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = NULL;
						$$ = (PGNode *) n;
					}
				| DISCARD TEMPORARY
					{
						PGDeallocateStmt *n = makeNode(PGDeallocateStmt);
						n->name = NULL;
						$$ = (PGNode *) n;
					}
		;
