/*
 *	Copyright 2021 Andrey Terekhov, Ilya Andreev
 *
 *	Licensed under the Apache License, Version 2.0 (the "License");
 *	you may not use this file except in compliance with the License.
 *	You may obtain a copy of the License at
 *
 *		http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */

#pragma once

#include "defs.h"
#include "errors.h"
#include "lexer.h"
#include "syntax.h"
#include "tree.h"
#include "uniio.h"


// #define GENERATE_TREE

#define TREE			(prs->sx->tree)
#define MAXLABELS		10000
#define MAXSTACKSIZE	100


#ifdef __cplusplus
extern "C" {
#endif

/** Type of operand on top of anonymous stack */
typedef enum OPERAND { variable, value, number, address } operand_t;
typedef struct operator { uint8_t precedence; token_t token; size_t addr; } operator_t;

typedef struct parser
{
	syntax *sx;							/**< Syntax structure */
	lexer *lxr;							/**< Lexer structure */
	node nd;							/**< Node for expression subtree */

	token_t token;						/**< Current token */

	size_t function_mode;				/**< Mode of currenty parsed function */
	size_t array_dimensions;			/**< Array dimensions counter */
	item_t labels[MAXLABELS];			/**< Labels table */
	size_t labels_counter;				/**< Labels counter */

	operator_t stackop[MAXSTACKSIZE];	/**< Operator stack */
	item_t stackoperands[MAXSTACKSIZE];	/**< Operands stack */
	size_t sp;							/**< Operators counter */

	item_t sopnd;						/**< Operands counter */
	operand_t anst;						/**< Type of the top operand of anonimous stack */
	item_t ansttype;					/**< Mode of the top operand of anonimous stack */

	item_t leftansttype;				/**< Mode of the LHS part of assignment expression */
	size_t lastid;						/**< Index of the last read identifier */
	item_t anstdispl;					/**< Displacement of the operand */
	int op; // TODO: убрать поле

	int func_def;						/**< @c 0 for function without arguments,
										 @c 1 for function definition,
										 @c 2 for function declaration,
										 @c 3 for others */

	int flag_strings_only;				/**< @c 0 for non-string initialization,
										 @c 1 for string initialization,
										 @c 2 for parsing before initialization */

	int flag_array_in_struct;			/**< Set, if parsed struct declaration has an array */
	int flag_empty_bounds;				/**< Set, if array declaration has empty bounds */
	int flag_was_return;				/**< Set, if was return in parsed function */
	int flag_in_switch;					/**< Set, if parser is in switch body */
	int flag_in_assignment;				/**< Set, if parser is in assignment */
	int flag_in_loop;					/**< Set, if parser is in loop body */
	int flag_was_type_def;				/**< Set, if was type definition */

	int was_error;						/**< Error flag */
} parser;

/**	The kind of block to parse */
typedef enum BLOCK
{
	REGBLOCK,
	THREAD,
	FUNCBODY
} block_t;


/**
 *	Parse source code to generate syntax structure
 *
 *	@param	io			Universal io structure
 *	@param	sx			Syntax structure
 *
 *	@return	@c 0 on success, @c 1 on failure
 */
int parse(universal_io *const io, syntax *const sx);


/**
 *	Emit an error from parser
 *
 *	@param	prs			Parser structure
 *	@param	num			Error code
 */
void parser_error(parser *const prs, error_t num, ...);


/**
 *	Consume the current 'peek token' and lex the next one
 *
 *	@param	prs			Parser structure
 */
void token_consume(parser *const prs);

/**
 *	Try to consume the current 'peek token' and lex the next one
 *
 *	@param	prs			Parser structure
 *	@param	expected	Expected token to consume
 *
 *	@return	@c 1 on consuming 'peek token', @c 0 on otherwise
 */
int token_try_consume(parser *const prs, const token_t expected);

/**
 *	Try to consume the current 'peek token' and lex the next one
 *	If 'peek token' is expected, parser will consume it, otherwise an error will be emitted
 *
 *	@param	prs			Parser structure
 *	@param	expected	Expected token to consume
 *	@param	err			Error to emit
 */
void token_expect_and_consume(parser *const prs, const token_t expected, const error_t err);

/**
 *	Read tokens until one of the specified tokens
 *
 *	@param	prs			Parser structure
 *	@param	tokens		Set of specified tokens
 */
void token_skip_until(parser *const prs, const uint8_t tokens);


/**
 *	Parse expression [C99 6.5.17]
 *
 *	expression:
 *		assignment-expression
 *		expression ',' assignment-expression
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 *
 *	@return	Type of parsed expression
 */
item_t parse_expression(parser *const prs, node *const parent);

/**
 *	Parse assignment expression [C99 6.5.16]
 *
 *	assignment-expression:
 *		conditional-expression
 *		unary-expression assignment-operator assignment-expression
 *
 *	assignment-operator: one of
 *		=  *=  /=  %=  +=  -=  <<=  >>=  &=  ˆ=  |=
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 *
 *	@return	Type of parsed expression
 */
item_t parse_assignment_expression(parser *const prs, node *const parent);

/**
 *	Parse expression in parentheses
 *
 *	parenthesized-expression:
 *		'(' expression ')'
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 *
 *	@return	Type of parsed expression
 */
item_t parse_parenthesized_expression(parser *const prs, node *const parent);

/**
 *	Parse constant expression [C99 6.6]
 *
 *	constant-expression:
 *		conditional-expression
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 *
 *	@return	Type of parsed expression
 */
item_t parse_constant_expression(parser *const prs, node *const parent);

/**
 *	Parse condition
 *	@note	must be evaluated to a simple value
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 *
 *	@return	Type of parsed expression
 */
item_t parse_condition(parser *const prs, node *const parent);

/**
 *	Parse string literal [C99 6.5.1]
 *
 *	primary-expression:
 *		string-literal
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 *
 *	@return	Type of parsed expression
 */
item_t parse_string_literal(parser *const prs, node *const parent);

/**
 *	Insert @c WIDEN node
 *
 *	@param	prs			Parser structure
 */
void parse_insert_widen(parser *const prs);


/**
 *	Parse a declaration [C99 6.7]
 *	@note Parses a full declaration, which consists of declaration-specifiers,
 *	some number of declarators, and a semicolon
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 */
void parse_declaration_inner(parser *const prs, node *const parent);

/**
 *	Parse a top level declaration [C99 6.7]
 *	@note Parses a full declaration, which consists either of declaration-specifiers,
 *	some number of declarators, and a semicolon, or function definition
 *
 *	@param	prs			Parser structure
 *	@param	root		Root node in AST
 */
void parse_declaration_external(parser *const prs, node *const root);

/**
 *	Parse initializer [C99 6.7.8]
 *
 *	initializer:
 *		assignment-expression
 *		'{' initializer-list '}'
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 *	@param	type		Type of variable in declaration
 */
void parse_initializer(parser *const prs, node *const parent, const item_t type);


/**
 *	Parse statement [C99 6.8]
 *
 *	statement:
 *		labeled-statement
 *		compound-statement
 *		expression-statement
 *		selection-statement
 *		iteration-statement
 *		jump-statement
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 */
void parse_statement(parser *const prs, node *const parent);

/**
 *	Parse '{}' block [C99 6.8.2]
 *
 *	compound-statement:
 *  	{ block-item-list[opt] }
 *
 *	block-item-list:
 *		block-item
 *		block-item-list block-item
 *
 *	block-item:
 *		declaration
 *		statement
 *
 *	@param	prs			Parser structure
 *	@param	parent		Parent node in AST
 */
void parse_statement_compound(parser *const prs, node *const parent, const block_t type);


/**
 *	Check if mode is function
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_function(syntax *const sx, const item_t mode);

/**
 *	Check if mode is array
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_array(syntax *const sx, const item_t mode);

/**
 *	Check if mode is string
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_string(syntax *const sx, const item_t mode);

/**
 *	Check if mode is pointer
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_pointer(syntax *const sx, const item_t mode);

/**
 *	Check if mode is struct
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_struct(syntax *const sx, const item_t mode);

/**
 *	Check if mode is floating point
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_float(const item_t mode);

/**
 *	Check if mode is integer
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_int(const item_t mode);

/**
 *	Check if mode is void
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_void(const item_t mode);

/**
 *	Check if mode is undefined
 *
 *	@param	sx			Syntax structure
 *	@param	mode		Mode for check
 *
 *	@return	@c 1 on true, @c 0 on false
 */
int mode_is_undefined(const item_t mode);


/**
 *	Add new item to identifiers table
 *
 *	@param	prs			Parser structure
 *	@param	repr		New identifier index in representations table
 *	@param	type		@c -1 for function as parameter,
 *						@c  0 for variable,
 *						@c  1 for label,
 *						@c  funcnum for function,
 *						@c  >= @c 100 for type specifier
 *	@param	mode		New identifier mode
 *
 *	@return	Index of the last item in identifiers table
 */
size_t to_identab(parser *const prs, const size_t repr, const item_t type, const item_t mode);

/**
 *	Add a new record to modes table
 *
 *	@param	prs			Parser structure
 *	@param	mode		@c mode_pointer or @c mode_array
 *	@param	element		Type of element
 *
 *	@return	Index of the new record in modes table, @c SIZE_MAX on failure
 */
item_t to_modetab(parser *const prs, const item_t mode, const item_t element);

/**
 *	Add a new node to expression subtree
 *
 *	@param	prs			Parser structure
 *	@param	op			New node type
 */
void to_tree(parser *const prs, const item_t op);

/**
 *	Create tree reference for backward compatibility
 *
 *	@param	prs			Parser structure
 *
 *	@return	Tree reference
 */
item_t tree_reference(parser *const prs);

#ifdef __cplusplus
} /* extern "C" */
#endif
