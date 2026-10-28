#include "native_abi.h"

Om_Status om_llvm_native_record_classify_darwin_arm64(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, Om_Llvm_Native_Record_Abi *abi)
{
    return om_llvm_native_record_classify_aapcs64_common(
        context, record_type, size, alignment, fields, field_count, false,
        abi);
}
