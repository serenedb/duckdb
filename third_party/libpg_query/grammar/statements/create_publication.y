/*****************************************************************************
 *
 *		QUERY :
 *				CREATE PUBLICATION name [ FOR ALL TABLES | FOR TABLE table_name [, ...] ]
 *
 *****************************************************************************/

CreatePublicationStmt:
			CREATE_P PUBLICATION ColId
				{
					PGCreatePublicationStmt *n = makeNode(PGCreatePublicationStmt);
					n->pubname = $3;
					n->tables = NIL;
					n->for_all_tables = false;
					n->options = NIL;
					$$ = (PGNode *)n;
				}
			| CREATE_P PUBLICATION ColId FOR ALL TABLES
				{
					PGCreatePublicationStmt *n = makeNode(PGCreatePublicationStmt);
					n->pubname = $3;
					n->tables = NIL;
					n->for_all_tables = true;
					n->options = NIL;
					$$ = (PGNode *)n;
				}
			| CREATE_P PUBLICATION ColId FOR TABLE publication_table_list
				{
					PGCreatePublicationStmt *n = makeNode(PGCreatePublicationStmt);
					n->pubname = $3;
					n->tables = $6;
					n->for_all_tables = false;
					n->options = NIL;
					$$ = (PGNode *)n;
				}
		;

publication_table_list:
			qualified_name
				{ $$ = list_make1($1); }
			| publication_table_list ',' qualified_name
				{ $$ = lappend($1, $3); }
		;
