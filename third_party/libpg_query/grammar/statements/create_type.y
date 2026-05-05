/*****************************************************************************
 *
 * Create Type Statement
 *
 *****************************************************************************/
CreateTypeStmt:
				CREATE_P OptTemp TYPE_P qualified_name AS create_type_value
				{
					PGCreateTypeStmt *n = (PGCreateTypeStmt *) $6;
					$4->relpersistence = $2;
					n->typeName = $4;
					n->onconflict = PG_ERROR_ON_CONFLICT;
					$$ = (PGNode *)n;
				}
				| CREATE_P OptTemp TYPE_P IF_P NOT EXISTS qualified_name AS create_type_value
				{
					PGCreateTypeStmt *n = (PGCreateTypeStmt *) $9;
					$7->relpersistence = $2;
					n->typeName = $7;
					n->onconflict = PG_IGNORE_ON_CONFLICT;
					$$ = (PGNode *)n;
				}
				| CREATE_P OR REPLACE OptTemp TYPE_P qualified_name AS create_type_value
				{
					PGCreateTypeStmt *n = (PGCreateTypeStmt *) $8;
					$6->relpersistence = $4;
					n->typeName = $6;
					n->onconflict = PG_REPLACE_ON_CONFLICT;
					$$ = (PGNode *)n;
				}
		;

create_type_value:
	ENUM_P select_with_parens
	{
		PGCreateTypeStmt *n = makeNode(PGCreateTypeStmt);
		n->kind = PG_NEWTYPE_ENUM;
		n->query = $2;
		n->vals = NULL;
		$$ = (PGNode *)n;
	}
	| ENUM_P '(' opt_enum_val_list ')'
	{
		PGCreateTypeStmt *n = makeNode(PGCreateTypeStmt);
		n->kind = PG_NEWTYPE_ENUM;
		n->vals = $3;
		n->query = NULL;
		$$ = (PGNode *)n;
	}
	| '(' composite_type_field_list ')'
	{
		PGCreateTypeStmt *n = makeNode(PGCreateTypeStmt);
		n->kind = PG_NEWTYPE_COMPOSITE;
		n->vals = $2;
		n->query = NULL;
		$$ = (PGNode *)n;
	}
	| Typename
	{
		PGCreateTypeStmt *n = makeNode(PGCreateTypeStmt);
		n->query = NULL;
		auto name = std::string(reinterpret_cast<PGValue *>($1->names->tail->data.ptr_value)->val.str);
		if (name == "enum") {
			n->kind = PG_NEWTYPE_ENUM;
			n->vals = $1->typmods;
		} else {
			n->kind = PG_NEWTYPE_ALIAS;
			n->ofType = $1;
		}
		$$ = (PGNode *)n;
	}
	;


composite_type_field:
	ColId Typename
	{
		PGColumnDef *n = makeNode(PGColumnDef);
		n->category = COL_STANDARD;
		n->colname = $1;
		n->typeName = $2;
		n->inhcount = 0;
		n->is_local = true;
		n->is_not_null = false;
		n->is_from_type = false;
		n->storage = 0;
		n->raw_default = NULL;
		n->cooked_default = NULL;
		n->collOid = InvalidOid;
		n->constraints = NULL;
		n->collClause = NULL;
		n->location = @1;
		$$ = (PGNode *)n;
	}
	;


composite_type_field_list:
	composite_type_field
		{ $$ = list_make1($1); }
	| composite_type_field_list ',' composite_type_field
		{ $$ = lappend($1, $3); }
	;


opt_enum_val_list:
			enum_val_list { $$ = $1;}
			|				{$$ = NIL;}
			;

enum_val_list: Sconst
				{
					$$ = list_make1(makeStringConst($1, @1));
				}
				| enum_val_list ',' Sconst
				{
					$$ = lappend($1, makeStringConst($3, @3));
				}
				;



