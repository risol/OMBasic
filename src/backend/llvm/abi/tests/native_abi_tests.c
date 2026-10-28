#include "native_abi.h"

#include <string.h>
#include <stdio.h>

#include "llvm-c/Analysis.h"

static unsigned failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            (void)fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__,      \
                          __LINE__, #condition);                                \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

static Om_Llvm_Native_Record_Field_Layout integer_field(LLVMTypeRef type,
                                                         uint64_t offset,
                                                         uint64_t size,
                                                         uint64_t alignment,
                                                         unsigned bit_width)
{
    return (Om_Llvm_Native_Record_Field_Layout){
        .llvm_type = type,
        .offset = offset,
        .size = size,
        .alignment = alignment,
        .bit_width = bit_width,
        .is_integer = true,
    };
}

static Om_Llvm_Native_Record_Field_Layout float_field(LLVMTypeRef type,
                                                       uint64_t offset,
                                                       uint64_t size,
                                                       unsigned bit_width)
{
    return (Om_Llvm_Native_Record_Field_Layout){
        .llvm_type = type,
        .offset = offset,
        .size = size,
        .alignment = size,
        .bit_width = bit_width,
        .is_float = true,
    };
}

static LLVMTypeRef record_type(LLVMContextRef context,
                               LLVMTypeRef *fields,
                               unsigned field_count)
{
    return LLVMStructTypeInContext(context, fields, field_count, 0);
}

static void test_aapcs_small_record(LLVMContextRef context)
{
    LLVMTypeRef i32_type = LLVMIntTypeInContext(context, 32);
    LLVMTypeRef fields[1] = {i32_type};
    const Om_Llvm_Native_Record_Field_Layout layout[1] = {
        integer_field(i32_type, 0, 4, 4, 32),
    };
    Om_Llvm_Native_Record_Abi abi = {0};
    CHECK(om_llvm_native_record_classify_aapcs64(context,
        record_type(context, fields, 1), 4, 4, layout, 1, &abi)
        == OM_STATUS_OK);
    CHECK(!abi.indirect);
    CHECK(abi.part_count == 1);
    CHECK(LLVMGetTypeKind(abi.parts[0].llvm_type) == LLVMIntegerTypeKind);
    CHECK(LLVMGetIntTypeWidth(abi.parts[0].llvm_type) == 64);
    CHECK(LLVMGetIntTypeWidth(abi.parts[0].memory_type) == 32);
    CHECK(abi.parts[0].mode == OM_LLVM_NATIVE_RECORD_PART_INTEGER_EXTEND);
    CHECK(LLVMGetIntTypeWidth(abi.result_type) == 32);
    CHECK(abi.result_part_count == 1);
}

static void test_aapcs_partial_array(LLVMContextRef context)
{
    LLVMTypeRef i32_type = LLVMIntTypeInContext(context, 32);
    LLVMTypeRef fields[3] = {i32_type, i32_type, i32_type};
    const Om_Llvm_Native_Record_Field_Layout layout[3] = {
        integer_field(i32_type, 0, 4, 4, 32),
        integer_field(i32_type, 4, 4, 4, 32),
        integer_field(i32_type, 8, 4, 4, 32),
    };
    Om_Llvm_Native_Record_Abi abi = {0};
    CHECK(om_llvm_native_record_classify_aapcs64(context,
        record_type(context, fields, 3), 12, 4, layout, 3, &abi)
        == OM_STATUS_OK);
    CHECK(abi.part_count == 1);
    CHECK(abi.parts[0].mode == OM_LLVM_NATIVE_RECORD_PART_INTEGER_ARRAY);
    CHECK(LLVMGetTypeKind(abi.parts[0].llvm_type) == LLVMArrayTypeKind);
    CHECK(LLVMGetArrayLength(abi.parts[0].llvm_type) == 2);
    CHECK(LLVMGetIntTypeWidth(LLVMGetElementType(abi.parts[0].llvm_type))
        == 64);
    CHECK(LLVMGetTypeKind(abi.result_type) == LLVMArrayTypeKind);
    CHECK(abi.result_part_count == 2);
    CHECK(abi.result_parts[0].memory_size == 8);
    CHECK(abi.result_parts[1].offset == 8);
    CHECK(abi.result_parts[1].memory_size == 4);
    CHECK(LLVMGetIntTypeWidth(abi.result_parts[1].memory_type) == 32);
}

static void test_aapcs_hfa(LLVMContextRef context)
{
    LLVMTypeRef f64_type = LLVMDoubleTypeInContext(context);
    LLVMTypeRef fields[4] = {f64_type, f64_type, f64_type, f64_type};
    Om_Llvm_Native_Record_Field_Layout layout[4];
    for (size_t index = 0; index < 4; ++index) {
        layout[index] = float_field(f64_type, (uint64_t)index * 8, 8, 64);
    }
    LLVMTypeRef record = record_type(context, fields, 4);
    Om_Llvm_Native_Record_Abi abi = {0};
    CHECK(om_llvm_native_record_classify_aapcs64(context, record, 32, 8,
        layout, 4, &abi) == OM_STATUS_OK);
    CHECK(!abi.indirect);
    CHECK(abi.part_count == 1);
    CHECK(LLVMGetTypeKind(abi.parts[0].llvm_type) == LLVMArrayTypeKind);
    CHECK(LLVMGetArrayLength(abi.parts[0].llvm_type) == 4);
    CHECK(LLVMGetElementType(abi.parts[0].llvm_type) == f64_type);
    CHECK(abi.parts[0].abi_alignment == 8);
    CHECK(abi.result_type == record);
    CHECK(abi.result_is_record);

    Om_Llvm_Native_Record_Abi darwin_abi = {0};
    CHECK(om_llvm_native_record_classify_darwin_arm64(context, record, 32, 8,
        layout, 4, &darwin_abi) == OM_STATUS_OK);
    CHECK(darwin_abi.parts[0].abi_alignment == 0);
    CHECK(darwin_abi.result_is_record);

    const unsigned alignstack_kind = LLVMGetEnumAttributeKindForName(
        "alignstack", sizeof("alignstack") - 1);
    CHECK(alignstack_kind != 0);
    LLVMAttributeRef alignstack = LLVMCreateEnumAttribute(
        context, alignstack_kind, abi.parts[0].abi_alignment);
    LLVMModuleRef module = LLVMModuleCreateWithNameInContext(
        "native_abi_hfa_attribute", context);
    LLVMTypeRef parameter_types[1] = {abi.parts[0].llvm_type};
    LLVMTypeRef signature = LLVMFunctionType(record, parameter_types, 1, 0);
    CHECK(alignstack != NULL);
    CHECK(module != NULL);
    CHECK(signature != NULL);
    if (module == NULL || signature == NULL) {
        if (module != NULL) {
            LLVMDisposeModule(module);
        }
        return;
    }
    LLVMValueRef function = LLVMAddFunction(module, "native_hfa", signature);
    CHECK(function != NULL);
    if (alignstack != NULL && function != NULL) {
        LLVMAddAttributeAtIndex(function, 1, alignstack);
        char *ir = LLVMPrintModuleToString(module);
        CHECK(ir != NULL);
        if (ir != NULL) {
            CHECK(strstr(ir, "alignstack(8)") != NULL);
            LLVMDisposeMessage(ir);
        }
        char *verification_message = NULL;
        CHECK(LLVMVerifyModule(module, LLVMReturnStatusAction,
                               &verification_message) == 0);
        if (verification_message != NULL) {
            LLVMDisposeMessage(verification_message);
        }
    }
    if (module != NULL) {
        LLVMDisposeModule(module);
    }
}

static void test_aapcs_indirect_and_mixed(LLVMContextRef context)
{
    LLVMTypeRef i64_type = LLVMIntTypeInContext(context, 64);
    LLVMTypeRef large_fields[3] = {i64_type, i64_type, i64_type};
    const Om_Llvm_Native_Record_Field_Layout large_layout[3] = {
        integer_field(i64_type, 0, 8, 8, 64),
        integer_field(i64_type, 8, 8, 8, 64),
        integer_field(i64_type, 16, 8, 8, 64),
    };
    Om_Llvm_Native_Record_Abi large_abi = {0};
    CHECK(om_llvm_native_record_classify_aapcs64(context,
        record_type(context, large_fields, 3), 24, 8, large_layout, 3,
        &large_abi) == OM_STATUS_OK);
    CHECK(large_abi.indirect);
    CHECK(large_abi.byval);

    LLVMTypeRef i32_type = LLVMIntTypeInContext(context, 32);
    LLVMTypeRef f32_type = LLVMFloatTypeInContext(context);
    LLVMTypeRef mixed_fields[2] = {i32_type, f32_type};
    const Om_Llvm_Native_Record_Field_Layout mixed_layout[2] = {
        integer_field(i32_type, 0, 4, 4, 32),
        float_field(f32_type, 4, 4, 32),
    };
    Om_Llvm_Native_Record_Abi mixed_abi = {0};
    CHECK(om_llvm_native_record_classify_aapcs64(context,
        record_type(context, mixed_fields, 2), 8, 4, mixed_layout, 2,
        &mixed_abi) == OM_STATUS_OK);
    CHECK(!mixed_abi.indirect);
    CHECK(mixed_abi.part_count == 1);
    CHECK(LLVMGetIntTypeWidth(mixed_abi.parts[0].llvm_type) == 64);
    CHECK(LLVMGetIntTypeWidth(mixed_abi.result_type) == 64);
}

int main(void)
{
    LLVMContextRef context = LLVMContextCreate();
    if (context == NULL) {
        (void)fprintf(stderr, "could not create LLVM context\n");
        return 1;
    }
    test_aapcs_small_record(context);
    test_aapcs_partial_array(context);
    test_aapcs_hfa(context);
    test_aapcs_indirect_and_mixed(context);
    LLVMContextDispose(context);
    if (failures != 0) {
        (void)fprintf(stderr, "%u native ABI check(s) failed\n", failures);
        return 1;
    }
    (void)puts("AAPCS64 and Darwin ARM64 native ABI checks passed");
    return 0;
}
