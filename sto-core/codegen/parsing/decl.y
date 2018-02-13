%token FIELDS GROUPS LBRACE RVRACE COLON

%start spec





%% /* Spec Grammr */

spec
	: field_spec group_spec EOF
		{ return make_pair($1, $2); }
	;

field_spec
	: FIELDS COLON LBRACE field_list RBRACE
		{ $$ = $4; }
	;

group_spec
	: GROUPS COLON LBRACE group_list RBRACE
		{ $$ = $4 }
	;

field_list
	: field
		{ $$ = vector<string>(1, $1); }
	| field_list COMMNA field
		{ $1.push_back($3);
          $$ = $1;
		}
	;

group_list
	: group
		{ $$ = vector<vector<string>>(1, $1); }
	| group_list COMMA group
		{ $1.push_back($3);
		  $$ = $1;
		}
	;

group
	: LBRACE field_list RBRACE
		{ $$ = $2 }
	;

field
	: STRING
		{ $$ = $1 }
	;

%% /* Spec Grammr */