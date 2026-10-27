#ifndef OMBASIC_FOUNDATION_IDS_H
#define OMBASIC_FOUNDATION_IDS_H

#include <stdint.h>

typedef struct Om_Source_Id {
    uint32_t value;
} Om_Source_Id;

typedef struct Om_Module_Id {
    uint32_t value;
} Om_Module_Id;

typedef struct Om_Symbol_Id {
    uint32_t value;
} Om_Symbol_Id;

typedef struct Om_Type_Id {
    uint32_t value;
} Om_Type_Id;

typedef struct Om_Scope_Id {
    uint32_t value;
} Om_Scope_Id;

typedef struct Om_Node_Id {
    uint32_t value;
} Om_Node_Id;

typedef struct Om_Ast_Node_Id {
    uint32_t value;
} Om_Ast_Node_Id;

#define OM_SOURCE_ID_INVALID ((Om_Source_Id){UINT32_MAX})
#define OM_MODULE_ID_INVALID ((Om_Module_Id){UINT32_MAX})
#define OM_SYMBOL_ID_INVALID ((Om_Symbol_Id){UINT32_MAX})
#define OM_TYPE_ID_INVALID ((Om_Type_Id){UINT32_MAX})
#define OM_SCOPE_ID_INVALID ((Om_Scope_Id){UINT32_MAX})
#define OM_NODE_ID_INVALID ((Om_Node_Id){UINT32_MAX})
#define OM_AST_NODE_ID_INVALID ((Om_Ast_Node_Id){UINT32_MAX})

#endif
