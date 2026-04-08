/*****************************************************************************
 *
 * CREATE FUNCTION stmt
 *
  *****************************************************************************/
CreateFunctionStmt:
		CREATE_P OptTemp macro_alias qualified_name table_macro_list
			{
				PGCreateFunctionStmt *n = makeNode(PGCreateFunctionStmt);
				$4->relpersistence = $2;
				n->name = $4;
				n->functions = $5;
				n->onconflict = PG_ERROR_ON_CONFLICT;
				$$ = (PGNode *)n;
			}
		| CREATE_P OptTemp macro_alias IF_P NOT EXISTS qualified_name table_macro_list
			{
				PGCreateFunctionStmt *n = makeNode(PGCreateFunctionStmt);
				$7->relpersistence = $2;
				n->name = $7;
				n->functions = $8;
				n->onconflict = PG_IGNORE_ON_CONFLICT;
				$$ = (PGNode *)n;
			}
		| CREATE_P OR REPLACE OptTemp macro_alias qualified_name table_macro_list
			{
				PGCreateFunctionStmt *n = makeNode(PGCreateFunctionStmt);
				$6->relpersistence = $4;
				n->name = $6;
				n->functions = $7;
				n->onconflict = PG_REPLACE_ON_CONFLICT;
				$$ = (PGNode *)n;
			}
		| CREATE_P OptTemp macro_alias qualified_name macro_definition_list
			{
					PGCreateFunctionStmt *n = makeNode(PGCreateFunctionStmt);
					$4->relpersistence = $2;
					n->name = $4;
					n->functions = $5;
					n->onconflict = PG_ERROR_ON_CONFLICT;
					$$ = (PGNode *)n;
			}
		| CREATE_P OptTemp macro_alias IF_P NOT EXISTS qualified_name macro_definition_list
			 {
				PGCreateFunctionStmt *n = makeNode(PGCreateFunctionStmt);
				$7->relpersistence = $2;
				n->name = $7;
				n->functions = $8;
				n->onconflict = PG_IGNORE_ON_CONFLICT;
				$$ = (PGNode *)n;
			 }
		| CREATE_P OR REPLACE OptTemp macro_alias qualified_name macro_definition_list
			 {
				PGCreateFunctionStmt *n = makeNode(PGCreateFunctionStmt);
				$6->relpersistence = $4;
				n->name = $6;
				n->functions = $7;
				n->onconflict = PG_REPLACE_ON_CONFLICT;
				$$ = (PGNode *)n;
			 }
	;

table_macro_definition:
		param_list AS TABLE select_no_parens
			{
				PGFunctionDefinition *n = makeNode(PGFunctionDefinition);
				n->params = $1;
				n->query = $4;
				$$ = (PGNode *)n;
			}
		| param_list pg_function_decorators BEGIN_P ATOMIC select_no_parens ';' END_P
			{
				PGFunctionDefinition *n = makeNode(PGFunctionDefinition);
				n->params = $1;
				n->query = $5;
				$$ = (PGNode *)n;
			}
		| param_list pg_function_decorators BEGIN_P ATOMIC select_no_parens END_P
			{
				PGFunctionDefinition *n = makeNode(PGFunctionDefinition);
				n->params = $1;
				n->query = $5;
				$$ = (PGNode *)n;
			}
	;

table_macro_definition_parens:
		param_list AS TABLE select_with_parens
			{
				PGFunctionDefinition *n = makeNode(PGFunctionDefinition);
				n->params = $1;
				n->query = $4;
				$$ = (PGNode *)n;
			}
	;

table_macro_list_internal:
		table_macro_definition_parens
			{
				$$ = list_make1($1);
			}
		| table_macro_list_internal ',' table_macro_definition_parens
			{
				$$ = lappend($1, $3);
			}
	;

table_macro_list:
		table_macro_definition
			{
				$$ = list_make1($1);
			}
		| table_macro_list_internal
	;

macro_definition:
		param_list AS a_expr
			{
				PGFunctionDefinition *n = makeNode(PGFunctionDefinition);
				n->params = $1;
				n->function = $3;
				$$ = (PGNode *)n;
			}
		| param_list pg_function_decorators RETURN a_expr
			{
				PGFunctionDefinition *n = makeNode(PGFunctionDefinition);
				n->params = $1;
				n->function = $4;
				$$ = (PGNode *)n;
			}
		| param_list pg_function_decorators AS Sconst
			{
				PGFunctionDefinition *n = makeNode(PGFunctionDefinition);
				n->params = $1;
				n->function = NULL;
				n->query = NULL;
				n->pg_body = $4;
				$$ = (PGNode *)n;
			}
	;

macro_definition_list:
		macro_definition
			{
				$$ = list_make1($1);
			}
		| macro_definition_list ',' macro_definition
			{
				$$ = lappend($1, $3);
			}
	;

macro_alias:
		FUNCTION
		| MACRO
	;

/*****************************************************************************
 * PG-style function decorators (parsed and ignored)
 *****************************************************************************/
pg_function_decorators:
		pg_function_decorator_list                           {}
	;

pg_function_decorator_list:
		pg_function_decorator                                {}
		| pg_function_decorator_list pg_function_decorator   {}
	;

pg_function_decorator:
		RETURNS Typename                                     {}
		| RETURNS TABLE '(' TableFuncElementList ')'         {}
		| RETURNS NULL_P ON NULL_P INPUT_P                   {}
		| CALLED ON NULL_P INPUT_P                           {}
		| LANGUAGE ColId                                     {}
		| IMMUTABLE                                          {}
		| STABLE                                             {}
		| VOLATILE                                           {}
		| STRICT                                             {}
		| LEAKPROOF                                          {}
		| NOT LEAKPROOF                                      {}
		| PARALLEL ColId                                     {}
		| COST NumericOnly                                   {}
		| ROWS NumericOnly                                   {}
		| SECURITY INVOKER                                   {}
		| SECURITY DEFINER                                   {}
		| EXTERNAL SECURITY INVOKER                          {}
		| EXTERNAL SECURITY DEFINER                          {}
	;


param_list:
		'(' ')'
			{
				$$ = NIL;
			}
		| '(' MacroParameterList ',' ')'
			{
				$$ = $2;
			}
		| '(' MacroParameterList ')'
			{
				$$ = $2;
			}
	;

MacroParameterList:
		MacroParameter
			{
				$$ = list_make1($1);
			}
		| MacroParameterList ',' MacroParameter
			{
				$$ = lappend($1, $3);
			}
	;

MacroParameter:
		param_name opt_Typename
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $1;
				n->typeName = $2;
				n->defaultValue = NULL;
				$$ = (PGNode *) n;
			}
		| param_name opt_Typename COLON_EQUALS a_expr
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $1;
				n->typeName = $2;
				n->defaultValue = (PGExpr *) $4;
				$$ = (PGNode *) n;
			}
		| param_name opt_Typename EQUALS_GREATER a_expr
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $1;
				n->typeName = $2;
				n->defaultValue = (PGExpr *) $4;
				$$ = (PGNode *) n;
			}
		| param_name opt_Typename DEFAULT a_expr
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $1;
				n->typeName = $2;
				n->defaultValue = (PGExpr *) $4;
				$$ = (PGNode *) n;
			}
		| param_name opt_Typename '=' a_expr
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $1;
				n->typeName = $2;
				n->defaultValue = (PGExpr *) $4;
				$$ = (PGNode *) n;
			}
		| IN_P param_name opt_Typename
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $2;
				n->typeName = $3;
				n->defaultValue = NULL;
				$$ = (PGNode *) n;
			}
		| IN_P param_name opt_Typename DEFAULT a_expr
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $2;
				n->typeName = $3;
				n->defaultValue = (PGExpr *) $5;
				$$ = (PGNode *) n;
			}
		| IN_P param_name opt_Typename '=' a_expr
			{
				PGFunctionParameter *n = makeNode(PGFunctionParameter);
				n->name = $2;
				n->typeName = $3;
				n->defaultValue = (PGExpr *) $5;
				$$ = (PGNode *) n;
			}
	;
