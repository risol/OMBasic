#ifndef OMBASIC_SYNTAX_AST_H
#define OMBASIC_SYNTAX_AST_H

#include <stddef.h>
#include <stdint.h>

#include "ombasic/foundation/ids.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/source/span.h"
#include "ombasic/syntax/lexer.h"

typedef enum Om_Ast_Kind {
    OM_AST_PROGRAM = 0,
    OM_AST_IMPORT,
    OM_AST_RECORD_DECLARATION,
    OM_AST_MESSAGE_DECLARATION,
    OM_AST_FUNCTION_DECLARATION,
    OM_AST_ACTION_DECLARATION,
    OM_AST_ACTOR_DECLARATION,
    OM_AST_RECEIVE_DECLARATION,
    OM_AST_PARAMETER,
    OM_AST_BLOCK,
    OM_AST_BINDING,
    OM_AST_RETURN,
    OM_AST_BREAK,
    OM_AST_CONTINUE,
    OM_AST_SEND,
    OM_AST_WATCH,
    OM_AST_STOP,
    OM_AST_CALL_STATEMENT,
    OM_AST_IF,
    OM_AST_IF_ARM,
    OM_AST_WHILE,
    OM_AST_FOR,
    OM_AST_SELECT,
    OM_AST_CASE_ARM,
    OM_AST_NAME,
    OM_AST_INTEGER_LITERAL,
    OM_AST_FLOAT_LITERAL,
    OM_AST_STRING_LITERAL,
    OM_AST_STRING_TEXT,
    OM_AST_INTERPOLATED_STRING,
    OM_AST_BOOLEAN_LITERAL,
    OM_AST_NULL_LITERAL,
    OM_AST_SELF,
    OM_AST_DOWN_REASON_LITERAL,
    OM_AST_UNARY,
    OM_AST_BINARY,
    OM_AST_CALL,
    OM_AST_MEMBER,
    OM_AST_INDEX,
    OM_AST_WITH,
    OM_AST_FIELD_UPDATE,
    OM_AST_SPAWN,
    OM_AST_GROUP,
    OM_AST_LIST,
    OM_AST_MAP,
    OM_AST_MAP_ENTRY
} Om_Ast_Kind;

typedef enum Om_Ast_Flag {
    OM_AST_FLAG_NONE = 0,
    OM_AST_FLAG_MUTABLE = 1u << 0,
    OM_AST_FLAG_ENTITY_NAME = 1u << 1,
    OM_AST_FLAG_INVALID_NAME = 1u << 2,
    OM_AST_FLAG_HAS_ELSE = 1u << 3,
    OM_AST_FLAG_FOR_NUMERIC = 1u << 4,
    OM_AST_FLAG_FOR_EACH = 1u << 5,
    OM_AST_FLAG_CASE_ELSE = 1u << 6,
    OM_AST_FLAG_MIN_INTEGER = 1u << 7,
    OM_AST_FLAG_EXPORTED = 1u << 8
} Om_Ast_Flag;

/*
 * Nodes are syntax-only. Child ranges refer to Ast.children and are immutable
 * after parsing. Name and lexeme views borrow the source retained by the caller.
 * Function/action children are parameters followed by a block; RECEIVE uses the
 * same layout. IF children are arms then an optional ELSE block. FOR children
 * are start/end/optional step/body or iterable/body. SELECT children are the
 * scrutinee followed by CASE arms. Calls contain callee then arguments.
 */
typedef struct Om_Ast_Node {
    Om_Ast_Kind kind;
    Om_Source_Span span;
    Om_String_View name;
    Om_Token_Kind token_kind;
    Om_Keyword keyword;
    uint32_t flags;
    uint32_t auxiliary;
    uint32_t first_child;
    uint32_t child_count;
    uint64_t integer_magnitude;
    double float_value;
} Om_Ast_Node;

typedef struct Om_Ast {
    const Om_Ast_Node *nodes;
    size_t node_count;
    const Om_Ast_Node_Id *children;
    size_t child_count;
    Om_Ast_Node_Id root;
    void *owned_nodes;
    void *owned_children;
} Om_Ast;

const Om_Ast_Node *om_ast_get_node(const Om_Ast *ast, Om_Ast_Node_Id id);
Om_Ast_Node_Id om_ast_get_child(const Om_Ast *ast, const Om_Ast_Node *node, size_t index);
void om_ast_destroy(Om_Ast *ast);

#endif
