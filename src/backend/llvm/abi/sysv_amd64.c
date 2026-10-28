#include "native_abi.h"

#include <limits.h>
#include <string.h>

typedef struct Om_Sysv_Amd64_Chunk
{
    bool used;
    bool has_integer;
    bool has_float;
    bool has_pointer;
    bool all_single_float;
    size_t field_count;
    LLVMTypeRef single_type;
    unsigned single_bit_width;
    bool single_signed;
    uint64_t begin;
    uint64_t end;
    uint64_t second_float_offset;
} Om_Sysv_Amd64_Chunk;

static unsigned part_alignment(uint64_t record_alignment, uint64_t offset)
{
    const uint64_t offset_alignment =
        offset == 0 ? record_alignment : offset & (~offset + 1);
    const uint64_t alignment = record_alignment < offset_alignment
                                   ? record_alignment
                                   : offset_alignment;
    return alignment > UINT_MAX ? UINT_MAX : (unsigned)alignment;
}

Om_Status om_llvm_native_record_classify_sysv_amd64(
    LLVMContextRef context, LLVMTypeRef record_type, uint64_t size,
    uint64_t alignment, const Om_Llvm_Native_Record_Field_Layout *fields,
    size_t field_count, Om_Llvm_Native_Record_Abi *abi)
{
    if (context == NULL || record_type == NULL || fields == NULL ||
        field_count == 0 || abi == NULL || size == 0 || alignment == 0)
    {
        return OM_STATUS_INVALID_ARGUMENT;
    }
    (void)memset(abi, 0, sizeof(*abi));
    abi->llvm_type = record_type;
    abi->size = size;
    abi->alignment = alignment;
    if (size > 16)
    {
        abi->indirect = true;
        abi->byval = true;
        return OM_STATUS_OK;
    }

    Om_Sysv_Amd64_Chunk chunks[2] = {{0}, {0}};
    for (size_t index = 0; index < field_count; ++index)
    {
        const Om_Llvm_Native_Record_Field_Layout *field = &fields[index];
        if (field->llvm_type == NULL || field->size == 0 ||
            field->alignment == 0 || field->offset % field->alignment != 0 ||
            field->size > UINT64_MAX - field->offset ||
            field->offset + field->size > size ||
            field->offset / 8 != (field->offset + field->size - 1) / 8)
        {
            abi->indirect = true;
            abi->byval = true;
            return OM_STATUS_OK;
        }
        const size_t chunk_index = (size_t)(field->offset / 8);
        if (chunk_index >= 2)
        {
            abi->indirect = true;
            abi->byval = true;
            return OM_STATUS_OK;
        }
        Om_Sysv_Amd64_Chunk *chunk = &chunks[chunk_index];
        if (!chunk->used)
        {
            chunk->used = true;
            chunk->begin = field->offset;
            chunk->end = field->offset + field->size;
            chunk->all_single_float = field->is_float;
            chunk->single_type = field->llvm_type;
            chunk->single_bit_width = field->bit_width;
            chunk->single_signed = field->is_signed;
        }
        else
        {
            if (field->offset < chunk->begin)
            {
                chunk->begin = field->offset;
            }
            if (field->offset + field->size > chunk->end)
            {
                chunk->end = field->offset + field->size;
            }
            if (!field->is_float)
            {
                chunk->all_single_float = false;
            }
            if (field->is_float && !chunk->has_integer &&
                chunk->field_count == 1 && field->bit_width == 32 &&
                chunk->single_bit_width == 32 &&
                field->offset == chunk->begin + 4)
            {
                chunk->second_float_offset = field->offset;
            }
            else if (field->is_float)
            {
                chunk->all_single_float = false;
            }
        }
        if (field->is_float)
        {
            chunk->has_float = true;
        }
        else if (field->is_integer || field->is_pointer)
        {
            chunk->has_integer = true;
            chunk->has_pointer = chunk->has_pointer || field->is_pointer;
            chunk->all_single_float = false;
        }
        else
        {
            return OM_STATUS_INVALID_ARGUMENT;
        }
        chunk->field_count += 1;
    }

    for (size_t index = 0; index < 2; ++index)
    {
        Om_Sysv_Amd64_Chunk *chunk = &chunks[index];
        if (!chunk->used)
        {
            continue;
        }
        Om_Llvm_Native_Record_Part *part = &abi->parts[abi->part_count++];
        part->offset = (uint64_t)index * 8;
        part->alignment = part_alignment(alignment, part->offset);
        if (chunk->has_integer)
        {
            const uint64_t byte_width = chunk->end - chunk->begin;
            if (byte_width == 0 || byte_width > 8 ||
                byte_width > UINT_MAX / CHAR_BIT)
            {
                abi->indirect = true;
                abi->byval = true;
                return OM_STATUS_OK;
            }
            part->bit_width = (unsigned)(byte_width * CHAR_BIT);
            if (chunk->field_count == 1 && chunk->has_pointer)
            {
                part->llvm_type = chunk->single_type;
                part->bit_width = chunk->single_bit_width;
            }
            else
            {
                part->llvm_type =
                    LLVMIntTypeInContext(context, part->bit_width);
            }
            part->is_integer = true;
            part->is_signed = chunk->single_signed;
        }
        else if (chunk->field_count == 1)
        {
            part->llvm_type = chunk->single_type;
            part->bit_width = chunk->single_bit_width;
        }
        else if (chunk->all_single_float && chunk->field_count == 2 &&
                 chunk->single_bit_width == 32 &&
                 chunk->second_float_offset == chunk->begin + 4)
        {
            part->llvm_type =
                LLVMVectorType(LLVMFloatTypeInContext(context), 2);
        }
        else
        {
            abi->indirect = true;
            abi->byval = true;
            return OM_STATUS_OK;
        }
        if (part->llvm_type == NULL)
        {
            return OM_STATUS_OUT_OF_MEMORY;
        }
    }
    return abi->part_count != 0 ? OM_STATUS_OK : OM_STATUS_INVALID_ARGUMENT;
}
