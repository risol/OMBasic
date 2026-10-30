#ifndef OMBASIC_SEMANTIC_NAME_RESOLVER_H
#define OMBASIC_SEMANTIC_NAME_RESOLVER_H

#include <stdbool.h>
#include <stddef.h>

#include "ombasic/foundation/ids.h"
#include "ombasic/foundation/status.h"
#include "ombasic/foundation/string_view.h"
#include "ombasic/module_graph/module_loader.h"
#include "ombasic/native/metadata/omi.h"
#include "ombasic/syntax/parser.h"

typedef enum Om_Symbol_Kind {
    OM_SYMBOL_BUILTIN = 0,
    OM_SYMBOL_MODULE_ALIAS,
    OM_SYMBOL_FUNCTION,
    OM_SYMBOL_ACTION,
    OM_SYMBOL_RECORD,
    OM_SYMBOL_MESSAGE,
    OM_SYMBOL_ACTOR,
    OM_SYMBOL_PARAMETER,
    OM_SYMBOL_IMMUTABLE_BINDING,
    OM_SYMBOL_MUTABLE_BINDING,
    OM_SYMBOL_LOOP_VARIABLE,
    OM_SYMBOL_FIELD
} Om_Symbol_Kind;

/* Stable semantic identities for language-provided prelude declarations.
   Spelling is resolved to these values when the prelude is registered. */
typedef enum Om_Builtin_Id {
    OM_BUILTIN_NONE = 0,
    OM_BUILTIN_UNIT,
    OM_BUILTIN_RESULT,
    OM_BUILTIN_NODE_REF,
    OM_BUILTIN_ACTOR_ID,
    OM_BUILTIN_ACTOR_DOWN_REASON,
    OM_BUILTIN_OK,
    OM_BUILTIN_ERROR,
    OM_BUILTIN_ACTOR_DOWN,
    OM_BUILTIN_PRINT,
    OM_BUILTIN_SYSTEM_ACTOR_CONSOLE,
    OM_BUILTIN_SYSTEM_ACTOR_TIMER,
    OM_BUILTIN_SYSTEM_ACTOR_FILE_SYSTEM,
    OM_BUILTIN_SYSTEM_ACTOR_CLOCK,
    OM_BUILTIN_SYSTEM_ACTOR_SECURE_RANDOM,
    OM_BUILTIN_SYSTEM_ACTOR_LOGGER,
    OM_BUILTIN_SYSTEM_ACTOR_NETWORK,
    OM_BUILTIN_SYSTEM_ACTOR_PROCESS,
    OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE,
    OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_WRITE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_READ_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_WRITE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_TIMER_AFTER,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_APPEND,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_APPEND_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_EXISTS,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_EXISTS_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_DELETE,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_DELETE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_CREATE,
    OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_CREATE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_LIST,
    OM_BUILTIN_SYSTEM_MESSAGE_DIRECTORY_LIST_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_STAT,
    OM_BUILTIN_SYSTEM_MESSAGE_FILE_STAT_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_MONOTONIC_NOW,
    OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_MONOTONIC_NOW_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_UTC_NOW,
    OM_BUILTIN_SYSTEM_MESSAGE_CLOCK_UTC_NOW_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_READ_LINE,
    OM_BUILTIN_SYSTEM_MESSAGE_CONSOLE_READ_LINE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_SECURE_RANDOM_BYTES,
    OM_BUILTIN_SYSTEM_MESSAGE_SECURE_RANDOM_BYTES_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_WRITE,
    OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_WRITE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_FLUSH,
    OM_BUILTIN_SYSTEM_MESSAGE_LOGGER_FLUSH_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_LISTEN,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_LISTEN_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_CONNECT,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_CONNECT_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_ACCEPT,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_ACCEPT_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_SEND,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_SEND_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_RECEIVE,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_RECEIVE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_CLOSE,
    OM_BUILTIN_SYSTEM_MESSAGE_TCP_CLOSE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_SPAWN,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_SPAWN_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WRITE_STDIN,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WRITE_STDIN_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_CLOSE_STDIN,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_CLOSE_STDIN_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDOUT,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDOUT_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDERR,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_READ_STDERR_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_TERMINATE,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_TERMINATE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WAIT,
    OM_BUILTIN_SYSTEM_MESSAGE_PROCESS_WAIT_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_CONNECT,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_CONNECT_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_SEND,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_SEND_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_RECEIVE,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_RECEIVE_COMPLETED,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_CLOSE,
    OM_BUILTIN_SYSTEM_MESSAGE_UDP_CLOSE_COMPLETED
} Om_Builtin_Id;

typedef enum Om_Builtin_Member_Id {
    OM_BUILTIN_MEMBER_NONE = 0,
    OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NORMAL,
    OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_FAILED,
    OM_BUILTIN_MEMBER_ACTOR_DOWN_REASON_NODE_UNREACHABLE
} Om_Builtin_Member_Id;

typedef enum Om_Builtin_Category {
    OM_BUILTIN_CATEGORY_CORE = 0,
    OM_BUILTIN_CATEGORY_RUNTIME_SERVICE,
    OM_BUILTIN_CATEGORY_RUNTIME_PRIMITIVE
} Om_Builtin_Category;

typedef struct Om_Builtin_Descriptor {
    Om_Builtin_Id id;
    Om_String_View spelling;
    Om_Builtin_Category category;
    Om_String_View type_rule;
    Om_String_View lowering_or_protocol;
    Om_String_View standard_rule;
} Om_Builtin_Descriptor;

typedef struct Om_Builtin_Member_Descriptor {
    Om_Builtin_Member_Id id;
    Om_Builtin_Id owner;
    Om_String_View spelling;
    Om_String_View standard_rule;
    Om_String_View semantic_value;
} Om_Builtin_Member_Descriptor;

size_t om_builtin_descriptor_count(void);
const Om_Builtin_Descriptor *om_builtin_descriptor_at(size_t index);
size_t om_builtin_member_descriptor_count(void);
const Om_Builtin_Member_Descriptor *om_builtin_member_descriptor_at(size_t index);

bool om_builtin_id_is_system_actor(Om_Builtin_Id id);
bool om_builtin_id_is_system_message(Om_Builtin_Id id);

typedef enum Om_Scope_Kind {
    OM_SCOPE_PRELUDE = 0,
    OM_SCOPE_MODULE,
    OM_SCOPE_FUNCTION,
    OM_SCOPE_ACTION,
    OM_SCOPE_ACTOR,
    OM_SCOPE_RECEIVE,
    OM_SCOPE_BLOCK,
    OM_SCOPE_LOOP,
    OM_SCOPE_SELECT_ARM,
    OM_SCOPE_RECORD,
    OM_SCOPE_MESSAGE
} Om_Scope_Kind;

typedef enum Om_Resolution_Diagnostic_Code {
    OM_RESOLUTION_DIAGNOSTIC_E0301 = 301,
    OM_RESOLUTION_DIAGNOSTIC_E0302 = 302,
    OM_RESOLUTION_DIAGNOSTIC_E0303 = 303,
    OM_RESOLUTION_DIAGNOSTIC_E0304 = 304,
    OM_RESOLUTION_DIAGNOSTIC_E0305 = 305,
    OM_RESOLUTION_DIAGNOSTIC_E0306 = 306,
    OM_RESOLUTION_DIAGNOSTIC_E0307 = 307,
    OM_RESOLUTION_DIAGNOSTIC_E0308 = 308
} Om_Resolution_Diagnostic_Code;

typedef struct Om_Symbol {
    Om_Symbol_Id id;
    Om_String_View name;
    Om_Symbol_Kind kind;
    Om_Builtin_Id builtin_id;
    Om_Source_Span span;
    Om_Scope_Id scope_id;
    Om_Module_Id module_id;
    Om_Module_Id target_module;
    Om_Ast_Node_Id declaration_node;
    bool exported;
    bool mutable_binding;
    /* Non-NULL for a prelude function imported from Native metadata. The
       module must outlive resolution, inference, semantic analysis, and emit. */
    const Om_Omi_Module *native_module;
    size_t native_function_index;
    /* Valid for OM_SYMBOL_RECORD entries imported from Native metadata. */
    size_t native_record_index;
} Om_Symbol;

typedef struct Om_Scope {
    Om_Scope_Id id;
    Om_Scope_Id parent_id;
    Om_Scope_Kind kind;
    Om_Module_Id module_id;
    Om_Ast_Node_Id owner_node;
} Om_Scope;

typedef struct Om_Resolved_Node {
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    Om_Symbol_Id symbol_id;
    Om_Builtin_Member_Id builtin_member_id;
} Om_Resolved_Node;

typedef struct Om_Node_Scope {
    Om_Module_Id module_id;
    Om_Ast_Node_Id node_id;
    Om_Scope_Id scope_id;
} Om_Node_Scope;

typedef struct Om_Resolution_Diagnostic {
    Om_Resolution_Diagnostic_Code code;
    Om_Module_Id module_id;
    Om_Source_Span span;
    Om_Source_Span related_span;
    bool has_related_span;
} Om_Resolution_Diagnostic;

typedef struct Om_Module_Export {
    Om_Module_Id module_id;
    Om_String_View name;
    Om_Symbol_Id symbol_id;
} Om_Module_Export;

typedef struct Om_Resolver_Config {
    /* Additional prelude names, for example the standard library functions. */
    const Om_String_View *prelude_names;
    size_t prelude_name_count;
    /* Native declarations become typed prelude functions. These modules must
       remain alive until all compiler results that borrow their metadata are
       destroyed. Names that collide with the built-in/additional prelude fail. */
    const Om_Omi_Module *native_modules;
    size_t native_module_count;
    /* Native metadata visible only in the importing BASIC module. */
    const struct Om_Module_Native_Import *module_native_imports;
    size_t module_native_import_count;
} Om_Resolver_Config;

typedef struct Om_Module_Native_Import {
    Om_Module_Id module_id;
    const Om_Omi_Module *native_module;
    Om_Source_Span import_span;
    size_t metadata_index;
} Om_Module_Native_Import;

typedef struct Om_Resolution_Result {
    const Om_Symbol *symbols;
    size_t symbol_count;
    const Om_Scope *scopes;
    size_t scope_count;
    const Om_Resolved_Node *resolved_nodes;
    size_t resolved_node_count;
    const Om_Node_Scope *node_scopes;
    size_t node_scope_count;
    const Om_Resolution_Diagnostic *diagnostics;
    size_t diagnostic_count;
    const Om_Module_Export *exports;
    size_t export_count;
    void *owned_symbols;
    void *owned_scopes;
    void *owned_resolved_nodes;
    void *owned_node_scopes;
    void *owned_diagnostics;
    void *owned_exports;
} Om_Resolution_Result;

/*
 * parsed_modules[i] must be the syntax result for Om_Module_Id{i}. Name views
 * in the result borrow parser/project storage; keep both alive until the
 * resolution result is destroyed.
 */
Om_Status om_name_resolve_project(
    const Om_Module_Project *project,
    const Om_Parser_Result *parsed_modules,
    size_t parsed_module_count,
    const Om_Resolver_Config *config,
    Om_Resolution_Result *result
);
void om_resolution_result_destroy(Om_Resolution_Result *result);
Om_Symbol_Id om_resolution_symbol_for_node(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
);
Om_Builtin_Member_Id om_resolution_builtin_member_for_node(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
);
Om_Scope_Id om_resolution_scope_for_node(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id,
    Om_Ast_Node_Id node_id
);
const Om_Symbol *om_resolution_symbol_by_id(
    const Om_Resolution_Result *result,
    Om_Symbol_Id symbol_id
);
size_t om_resolution_module_export_count(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id
);
const Om_Module_Export *om_resolution_module_export_at(
    const Om_Resolution_Result *result,
    Om_Module_Id module_id,
    size_t index
);
const char *om_resolution_diagnostic_code_name(Om_Resolution_Diagnostic_Code code);

#endif
