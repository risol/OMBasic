#ifndef OMBASIC_IR_IDS_H
#define OMBASIC_IR_IDS_H

#include <stdint.h>

typedef struct Om_Hir_Node_Id {
    uint32_t value;
} Om_Hir_Node_Id;

typedef struct Om_Hir_Entity_Id {
    uint32_t value;
} Om_Hir_Entity_Id;

typedef struct Om_Function_Instance_Id {
    uint32_t value;
} Om_Function_Instance_Id;

typedef struct Om_Mir_Function_Id {
    uint32_t value;
} Om_Mir_Function_Id;

typedef struct Om_Mir_Block_Id {
    uint32_t value;
} Om_Mir_Block_Id;

typedef struct Om_Mir_Value_Id {
    uint32_t value;
} Om_Mir_Value_Id;

typedef struct Om_Mir_Instruction_Id {
    uint32_t value;
} Om_Mir_Instruction_Id;

#define OM_HIR_NODE_ID_INVALID ((Om_Hir_Node_Id){UINT32_MAX})
#define OM_HIR_ENTITY_ID_INVALID ((Om_Hir_Entity_Id){UINT32_MAX})
#define OM_FUNCTION_INSTANCE_ID_INVALID ((Om_Function_Instance_Id){UINT32_MAX})
#define OM_MIR_FUNCTION_ID_INVALID ((Om_Mir_Function_Id){UINT32_MAX})
#define OM_MIR_BLOCK_ID_INVALID ((Om_Mir_Block_Id){UINT32_MAX})
#define OM_MIR_VALUE_ID_INVALID ((Om_Mir_Value_Id){UINT32_MAX})
#define OM_MIR_INSTRUCTION_ID_INVALID ((Om_Mir_Instruction_Id){UINT32_MAX})

#endif
