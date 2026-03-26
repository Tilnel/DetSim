/* expr_parser.y - Grammar for expression evaluation */
%{
#define _GNU_SOURCE
#include <cstring>
#include <cstdio>

/* Need full definition for %union */
#include "expr_ast.hpp"

int yylex(void);
void yyerror(const char *s);
extern int yylineno;

extern ExprNode* g_parser_result;
extern const char* g_input_string;
%}

%union {
    long int_val;
    char* str_val;
    ExprNode* node_val;
}

/* Destructor rules to prevent memory leaks on parse errors */
%destructor { delete $$; } <node_val>
%destructor { free($$); } <str_val>

%token <int_val> NUMBER CHAR_LITERAL
%token <str_val> IDENTIFIER TRACEE_N

%token KW_SIZEOF KW_OFFSETOF KW_TYPEOF KW_STRUCT KW_UNION KW_ENUM
%token KW_INT KW_CHAR KW_SHORT KW_LONG KW_FLOAT KW_DOUBLE KW_VOID
%token KW_UNSIGNED KW_SIGNED KW_CONST KW_STATIC

%token ARROW EQ NE LE GE AND OR SHL SHR INC DEC
%token INVALID

%type <node_val> expression primary_expr postfix_expr unary_expr cast_expr
%type <node_val> multiplicative_expr additive_expr shift_expr relational_expr
%type <node_val> equality_expr and_expr xor_expr or_expr logical_and_expr logical_or_expr
%type <node_val> conditional_expr assignment_expr type_name
%type <str_val> type_specifier

%right '='
%right '?' ':'
%left OR
%left AND
%left '|'
%left '^'
%left '&'
%left EQ NE
%left '<' '>' LE GE
%left SHL SHR
%left '+' '-'
%left '*' '/' '%'
%left ARROW '.' '[' ']'

%%

input:
    assignment_expr { g_parser_result = $1; }
    ;

assignment_expr:
    conditional_expr
    | unary_expr '=' assignment_expr { $$ = new AssignNode($1, $3); }
    ;

conditional_expr:
    logical_or_expr
    | logical_or_expr '?' expression ':' conditional_expr { 
        $$ = new ConditionalNode($1, $3, $5); 
    }
    ;

logical_or_expr:
    logical_and_expr
    | logical_or_expr OR logical_and_expr { $$ = new BinaryOpNode(BinaryOp::OR, $1, $3); }
    ;

logical_and_expr:
    or_expr
    | logical_and_expr AND or_expr { $$ = new BinaryOpNode(BinaryOp::AND, $1, $3); }
    ;

or_expr:
    xor_expr
    | or_expr '|' xor_expr { $$ = new BinaryOpNode(BinaryOp::BITOR, $1, $3); }
    ;

xor_expr:
    and_expr
    | xor_expr '^' and_expr { $$ = new BinaryOpNode(BinaryOp::XOR, $1, $3); }
    ;

and_expr:
    equality_expr
    | and_expr '&' equality_expr { $$ = new BinaryOpNode(BinaryOp::BITAND, $1, $3); }
    ;

equality_expr:
    relational_expr
    | equality_expr EQ relational_expr { $$ = new BinaryOpNode(BinaryOp::EQ, $1, $3); }
    | equality_expr NE relational_expr { $$ = new BinaryOpNode(BinaryOp::NE, $1, $3); }
    ;

relational_expr:
    shift_expr
    | relational_expr '<' shift_expr { $$ = new BinaryOpNode(BinaryOp::LT, $1, $3); }
    | relational_expr '>' shift_expr { $$ = new BinaryOpNode(BinaryOp::GT, $1, $3); }
    | relational_expr LE shift_expr { $$ = new BinaryOpNode(BinaryOp::LE, $1, $3); }
    | relational_expr GE shift_expr { $$ = new BinaryOpNode(BinaryOp::GE, $1, $3); }
    ;

shift_expr:
    additive_expr
    | shift_expr SHL additive_expr { $$ = new BinaryOpNode(BinaryOp::SHL, $1, $3); }
    | shift_expr SHR additive_expr { $$ = new BinaryOpNode(BinaryOp::SHR, $1, $3); }
    ;

additive_expr:
    multiplicative_expr
    | additive_expr '+' multiplicative_expr { $$ = new BinaryOpNode(BinaryOp::ADD, $1, $3); }
    | additive_expr '-' multiplicative_expr { $$ = new BinaryOpNode(BinaryOp::SUB, $1, $3); }
    ;

multiplicative_expr:
    cast_expr
    | multiplicative_expr '*' cast_expr { $$ = new BinaryOpNode(BinaryOp::MUL, $1, $3); }
    | multiplicative_expr '/' cast_expr { $$ = new BinaryOpNode(BinaryOp::DIV, $1, $3); }
    | multiplicative_expr '%' cast_expr { $$ = new BinaryOpNode(BinaryOp::MOD, $1, $3); }
    ;

cast_expr:
    unary_expr
    | '(' type_name ')' cast_expr { $$ = new CastNode($2, $4); }
    ;

type_name:
    type_specifier { $$ = new TypeNode($1); free($1); }
    | IDENTIFIER { $$ = new TypeNode($1); free($1); }
    | KW_STRUCT IDENTIFIER {
        std::string type_str = std::string("struct ") + $2;
        free($2);
        $$ = new TypeNode(type_str);
    }
    | type_name '*' {
        std::string type_str = static_cast<TypeNode*>($1)->get_type() + " *";
        delete $1;
        $$ = new TypeNode(type_str);
    }
    ;

type_specifier:
    KW_INT      { $$ = strdup("int"); }
    | KW_CHAR   { $$ = strdup("char"); }
    | KW_SHORT  { $$ = strdup("short"); }
    | KW_LONG   { $$ = strdup("long"); }
    | KW_FLOAT  { $$ = strdup("float"); }
    | KW_DOUBLE { $$ = strdup("double"); }
    | KW_VOID   { $$ = strdup("void"); }
    | KW_UNSIGNED { $$ = strdup("unsigned"); }
    | KW_SIGNED   { $$ = strdup("signed"); }
    ;

unary_expr:
    postfix_expr
    | INC unary_expr { $$ = new PrefixOpNode(PrefixOp::PRE_INC, $2); }
    | DEC unary_expr { $$ = new PrefixOpNode(PrefixOp::PRE_DEC, $2); }
    | '&' unary_expr { $$ = new AddressOfNode($2); }
    | '*' unary_expr { $$ = new DereferenceNode($2); }
    | '+' unary_expr { $$ = $2; }
    | '-' unary_expr { $$ = new UnaryOpNode(UnaryOp::NEG, $2); }
    | '!' unary_expr { $$ = new UnaryOpNode(UnaryOp::NOT, $2); }
    | '~' unary_expr { $$ = new UnaryOpNode(UnaryOp::BITNOT, $2); }
    | KW_SIZEOF unary_expr { $$ = new SizeofExprNode($2); }
    | KW_SIZEOF '(' type_name ')' { 
        $$ = new SizeofTypeNode(static_cast<TypeNode*>($3)->get_type()); 
        delete $3;
    }
    | KW_OFFSETOF '(' type_name ',' IDENTIFIER ')' {
        $$ = new OffsetofNode(static_cast<TypeNode*>($3)->get_type(), $5);
        delete $3;
        free($5);
    }
    | KW_TYPEOF '(' expression ')' {
        $$ = new TypeofNode($3);
    }
    ;

postfix_expr:
    primary_expr
    | postfix_expr '[' expression ']' { $$ = new ArrayIndexNode($1, $3); }
    | postfix_expr '.' IDENTIFIER { $$ = new MemberAccessNode(MemberOp::DOT, $1, $3); free($3); }
    | postfix_expr ARROW IDENTIFIER { $$ = new MemberAccessNode(MemberOp::ARROW, $1, $3); free($3); }
    | postfix_expr INC { $$ = new PostfixOpNode(PostfixOp::POST_INC, $1); }
    | postfix_expr DEC { $$ = new PostfixOpNode(PostfixOp::POST_DEC, $1); }
    | TRACEE_N '(' expression ')' {
        int proc_id = atoi($1 + 6);
        free($1);
        $$ = new ProcessQualifiedNode(proc_id, $3);
    }
    ;

primary_expr:
    NUMBER { $$ = new NumberNode($1); }
    | CHAR_LITERAL { $$ = new NumberNode($1); }
    | IDENTIFIER { $$ = new VariableNode($1); free($1); }
    | '(' expression ')' { $$ = $2; }
    ;

expression:
    assignment_expr { $$ = $1; }
    ;

%%

void yyerror(const char *s) {
    fprintf(stderr, "Parse error: %s at line %d\n", s, yylineno);
}

int yywrap() {
    return 1;
}

/* Include AST definitions after union declaration */
#include "expr_ast.hpp"
