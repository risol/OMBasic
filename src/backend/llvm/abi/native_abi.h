#ifndef OM_BACKEND_LLVM_ABI_NATIVE_ABI_H
#define OM_BACKEND_LLVM_ABI_NATIVE_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "llvm-c/Core.h"

#include "ombasic/foundation/status.h"

typedef struct Om_Llvm_Native_Record_Field_Layout
{
    LLVMTypeRef llvm_type;
    uint64_t offset;
    uint64_t size;
    uint64_t alignment;
    unsigned bit_width;
    bool is_integer;
    bool is_signed;
    bool is_float;
    bool is_pointer;
} Om_Llvm_Native_Record_Field_Layout;

typedef enum Om_Llvm_Native_Record_Part_Mode
{
    OM_LLVM_NATIVE_RECORD_PART_DIRECT = 0,
    OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND,
    OM_LLVM_NATIVE_RECORD_PART_INTEGER_ARRAY
} Om_Llvm_Native_Record_Part_Mode;

typedef struct Om_Llvm_Native_Record_Part
{
    LLVMTypeRef llvm_type;
    LLVMTypeRef memory_type;
    uint64_t offset;
    uint64_t memory_size;
    unsigned bit_width;
    unsigned alignment;
    unsigned abi_alignment;
    Om_Llvm_Native_Record_Part_Mode mode;
    bool is_integer;
    bool is_signed;
} Om_Llvm_Native_Record_Part;

typedef struct Om_Llvm_Native_Record_Abi
{
    LLVMTypeRef llvm_type;
    Om_Llvm_Native_Record_Part parts[2];
    size_t part_count;
    LLVMTypeRef result_type;
    Om_Llvm_Native_Record_Part result_parts[2];
    size_t result_part_count;
    uint64_t size;
    uint64_t alignment;
    bool indirect;
    bool result_is_record;
    bool byval;
} Om_Llvm_Native_Record_Abi;

Om_Status om_llvm_native_record_classify_sysv_amd64(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, Om_Llvm_Native_Record_Abi *abi);

Om_Status om_llvm_native_record_classify_win64(LLVMContextRef context,
                                               LLVMTypeRef record_type,
                                               uint64_t size,
                                               uint64_t alignment,
                                               Om_Llvm_Native_Record_Abi *abi);

Om_Status om_llvm_native_record_classify_aapcs64(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, Om_Llvm_Native_Record_Abi *abi);

Om_Status om_llvm_native_record_classify_aapcs64_common(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, bool hfa_alignstack,
    Om_Llvm_Native_Record_Abi *abi);

Om_Status om_llvm_native_record_classify_darwin_arm64(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, Om_Llvm_Native_Record_Abi *abi);

#endif
