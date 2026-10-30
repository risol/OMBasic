#include "ombasic/foundation/memory.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    REGEX_OK = 0,
    REGEX_INVALID_PATTERN = 1,
    REGEX_UNSUPPORTED = 2,
    REGEX_RESOURCE_EXHAUSTED = 3,
    REGEX_INVALID_UTF8 = 4,
};

enum {
    REGEX_MAX_PATTERN_BYTES = 4096,
    REGEX_MAX_TEXT_BYTES = 1048576,
    REGEX_MAX_NODES = 8192,
    REGEX_MAX_INSTRUCTIONS = 8192,
    REGEX_MAX_CLASSES = 2048,
    REGEX_MAX_RANGES = 4096,
    REGEX_MAX_PATCHES = 16384,
    REGEX_MAX_DEPTH = 128,
    REGEX_STEP_LIMIT = 16000000,
};

typedef enum Regex_Node_Kind {
    REGEX_NODE_EMPTY,
    REGEX_NODE_LITERAL,
    REGEX_NODE_ANY,
    REGEX_NODE_CLASS,
    REGEX_NODE_BOL,
    REGEX_NODE_EOL,
    REGEX_NODE_CONCAT,
    REGEX_NODE_ALTERNATE,
    REGEX_NODE_STAR,
    REGEX_NODE_PLUS,
    REGEX_NODE_OPTIONAL,
} Regex_Node_Kind;

typedef enum Regex_Instruction_Kind {
    REGEX_INST_CHAR,
    REGEX_INST_ANY,
    REGEX_INST_CLASS,
    REGEX_INST_BOL,
    REGEX_INST_EOL,
    REGEX_INST_JUMP,
    REGEX_INST_SPLIT,
    REGEX_INST_MATCH,
} Regex_Instruction_Kind;

typedef struct Regex_Range {
    uint32_t first;
    uint32_t last;
} Regex_Range;

typedef struct Regex_Class {
    uint16_t first_range;
    uint16_t range_count;
    bool negate;
} Regex_Class;

typedef struct Regex_Node {
    Regex_Node_Kind kind;
    struct Regex_Node *left;
    struct Regex_Node *right;
    uint32_t value;
} Regex_Node;

typedef struct Regex_Instruction {
    Regex_Instruction_Kind kind;
    int32_t out;
    int32_t out2;
    uint32_t value;
} Regex_Instruction;

typedef struct Regex_Patch {
    int32_t instruction;
    int32_t field;
    int32_t next;
} Regex_Patch;

typedef struct Regex_Fragment {
    int32_t start;
    int32_t patch_head;
    int32_t patch_tail;
} Regex_Fragment;

typedef struct Regex_Parser {
    const uint8_t *pattern;
    size_t length;
    size_t cursor;
    size_t error_offset;
    int status;
    size_t node_count;
    size_t class_count;
    size_t range_count;
    Regex_Node *nodes[REGEX_MAX_NODES];
    Regex_Class classes[REGEX_MAX_CLASSES];
    Regex_Range ranges[REGEX_MAX_RANGES];
} Regex_Parser;

typedef struct Regex_Compiler {
    Regex_Instruction instructions[REGEX_MAX_INSTRUCTIONS];
    Regex_Patch patches[REGEX_MAX_PATCHES];
    size_t instruction_count;
    size_t patch_count;
    int status;
} Regex_Compiler;

static bool utf8_next(const uint8_t *bytes, size_t length, size_t *cursor,
                      uint32_t *value)
{
    if (*cursor >= length) return false;
    const uint8_t first = bytes[(*cursor)++];
    if (first < 0x80u) {
        *value = first;
        return true;
    }
    unsigned continuation_count = 0;
    uint32_t scalar = 0;
    uint32_t minimum = 0;
    if (first >= 0xc2u && first <= 0xdfu) {
        continuation_count = 1;
        scalar = (uint32_t)(first & 0x1fu);
        minimum = 0x80u;
    } else if (first >= 0xe0u && first <= 0xefu) {
        continuation_count = 2;
        scalar = (uint32_t)(first & 0x0fu);
        minimum = 0x800u;
    } else if (first >= 0xf0u && first <= 0xf4u) {
        continuation_count = 3;
        scalar = (uint32_t)(first & 0x07u);
        minimum = 0x10000u;
    } else {
        return false;
    }
    if (continuation_count > length - *cursor) return false;
    for (unsigned index = 0; index < continuation_count; ++index) {
        const uint8_t next = bytes[(*cursor)++];
        if ((next & 0xc0u) != 0x80u) return false;
        scalar = (scalar << 6) | (uint32_t)(next & 0x3fu);
    }
    if (scalar < minimum || scalar > 0x10ffffu
        || (scalar >= 0xd800u && scalar <= 0xdfffu)) {
        return false;
    }
    *value = scalar;
    return true;
}

static void parser_fail(Regex_Parser *parser, int status, size_t offset)
{
    if (parser->status == REGEX_OK) {
        parser->status = status;
        parser->error_offset = offset;
    }
}

static Regex_Node *new_node(Regex_Parser *parser, Regex_Node_Kind kind,
                            Regex_Node *left, Regex_Node *right,
                            uint32_t value)
{
    if (parser->node_count == REGEX_MAX_NODES) {
        parser_fail(parser, REGEX_RESOURCE_EXHAUSTED, parser->cursor);
        return NULL;
    }
    Regex_Node *node = om_alloc(sizeof(*node));
    if (node == NULL) {
        parser_fail(parser, REGEX_RESOURCE_EXHAUSTED, parser->cursor);
        return NULL;
    }
    node->kind = kind;
    node->left = left;
    node->right = right;
    node->value = value;
    parser->nodes[parser->node_count++] = node;
    return node;
}

static bool class_add_range(Regex_Parser *parser, uint32_t class_id,
                            uint32_t first, uint32_t last)
{
    if (class_id >= parser->class_count || first > last
        || parser->range_count == REGEX_MAX_RANGES) {
        parser_fail(parser, REGEX_RESOURCE_EXHAUSTED, parser->cursor);
        return false;
    }
    Regex_Class *value = &parser->classes[class_id];
    if (value->range_count == 0) {
        value->first_range = (uint16_t)parser->range_count;
    }
    parser->ranges[parser->range_count++] = (Regex_Range){first, last};
    ++value->range_count;
    return true;
}

static int32_t new_class(Regex_Parser *parser, bool negate)
{
    if (parser->class_count == REGEX_MAX_CLASSES) {
        parser_fail(parser, REGEX_RESOURCE_EXHAUSTED, parser->cursor);
        return -1;
    }
    const size_t index = parser->class_count++;
    parser->classes[index] = (Regex_Class){0};
    parser->classes[index].negate = negate;
    return (int32_t)index;
}

static bool parse_escape(Regex_Parser *parser, uint32_t *value,
                         uint32_t *category, bool in_class)
{
    const size_t escape_offset = parser->cursor;
    if (parser->cursor >= parser->length) {
        parser_fail(parser, REGEX_INVALID_PATTERN, escape_offset);
        return false;
    }
    const uint8_t code = parser->pattern[parser->cursor++];
    *category = 0;
    switch (code) {
    case 'n': *value = '\n'; return true;
    case 'r': *value = '\r'; return true;
    case 't': *value = '\t'; return true;
    case 'f': *value = '\f'; return true;
    case 'v': *value = '\v'; return true;
    case 'd': *category = 1; break;
    case 's': *category = 2; break;
    case 'w': *category = 4; break;
    case 'D': *category = 1; *value = 1; break;
    case 'S': *category = 2; *value = 1; break;
    case 'W': *category = 4; *value = 1; break;
    case 'b': case 'B': case 'p': case 'P':
        parser_fail(parser, REGEX_UNSUPPORTED, escape_offset);
        return false;
    case '0':
        parser_fail(parser, REGEX_UNSUPPORTED, escape_offset);
        return false;
    default:
        if (code >= '1' && code <= '9') {
            parser_fail(parser, REGEX_UNSUPPORTED, escape_offset);
            return false;
        }
        *value = code;
        return true;
    }
    if (in_class && *category != 0) return true;
    return true;
}

static Regex_Node *parse_expression(Regex_Parser *parser, unsigned depth);

static Regex_Node *parse_class(Regex_Parser *parser)
{
    const size_t class_offset = parser->cursor == 0 ? 0 : parser->cursor - 1;
    bool negate = false;
    if (parser->cursor < parser->length && parser->pattern[parser->cursor] == '^') {
        negate = true;
        ++parser->cursor;
    }
    const int32_t class_id = new_class(parser, negate);
    if (class_id < 0) return NULL;
    bool saw_item = false;
    bool closed = false;
    while (parser->cursor < parser->length) {
        if (parser->pattern[parser->cursor] == ']' && saw_item) {
            ++parser->cursor;
            closed = true;
            break;
        }
        uint32_t first = 0;
        uint32_t category = 0;
        if (parser->pattern[parser->cursor] == '\\') {
            ++parser->cursor;
            if (!parse_escape(parser, &first, &category, true)) return NULL;
            if (category != 0) {
                if (first != 0) {
                    parser_fail(parser, REGEX_UNSUPPORTED, parser->cursor - 1);
                    return NULL;
                }
                if (category & 1u) {
                    (void)class_add_range(parser, (uint32_t)class_id, '0', '9');
                }
                if (category & 2u) {
                    (void)class_add_range(parser, (uint32_t)class_id, '\t', '\r');
                    (void)class_add_range(parser, (uint32_t)class_id, ' ', ' ');
                }
                if (category & 4u) {
                    (void)class_add_range(parser, (uint32_t)class_id, '0', '9');
                    (void)class_add_range(parser, (uint32_t)class_id, 'A', 'Z');
                    (void)class_add_range(parser, (uint32_t)class_id, '_', '_');
                    (void)class_add_range(parser, (uint32_t)class_id, 'a', 'z');
                }
                saw_item = true;
                continue;
            }
        } else if (!utf8_next(parser->pattern, parser->length,
                              &parser->cursor, &first)) {
            parser_fail(parser, REGEX_INVALID_PATTERN, parser->cursor);
            return NULL;
        }
        saw_item = true;
        if (parser->cursor < parser->length
            && parser->pattern[parser->cursor] == '-'
            && parser->cursor + 1 < parser->length
            && parser->pattern[parser->cursor + 1] != ']') {
            ++parser->cursor;
            uint32_t last = 0;
            if (parser->pattern[parser->cursor] == '\\') {
                ++parser->cursor;
                category = 0;
                if (!parse_escape(parser, &last, &category, true)
                    || category != 0) {
                    parser_fail(parser, REGEX_INVALID_PATTERN, parser->cursor);
                    return NULL;
                }
            } else if (!utf8_next(parser->pattern, parser->length,
                                  &parser->cursor, &last)) {
                parser_fail(parser, REGEX_INVALID_PATTERN, parser->cursor);
                return NULL;
            }
            if (first > last) {
                parser_fail(parser, REGEX_INVALID_PATTERN, class_offset);
                return NULL;
            }
            (void)class_add_range(parser, (uint32_t)class_id, first, last);
        } else {
            (void)class_add_range(parser, (uint32_t)class_id, first, first);
        }
        if (parser->status != REGEX_OK) return NULL;
    }
    if (!closed) {
        parser_fail(parser, REGEX_INVALID_PATTERN, class_offset);
        return NULL;
    }
    return new_node(parser, REGEX_NODE_CLASS, NULL, NULL,
                     (uint32_t)class_id);
}

static Regex_Node *parse_atom(Regex_Parser *parser, unsigned depth)
{
    if (depth > REGEX_MAX_DEPTH) {
        parser_fail(parser, REGEX_RESOURCE_EXHAUSTED, parser->cursor);
        return NULL;
    }
    if (parser->cursor >= parser->length) return NULL;
    const size_t atom_offset = parser->cursor;
    const uint8_t byte = parser->pattern[parser->cursor++];
    if (byte == '(') {
        if (parser->cursor < parser->length && parser->pattern[parser->cursor] == '?') {
            parser_fail(parser, REGEX_UNSUPPORTED, atom_offset);
            return NULL;
        }
        Regex_Node *nested = parse_expression(parser, depth + 1);
        if (parser->cursor >= parser->length || parser->pattern[parser->cursor] != ')') {
            parser_fail(parser, REGEX_INVALID_PATTERN, atom_offset);
            return NULL;
        }
        ++parser->cursor;
        return nested;
    }
    if (byte == '[') return parse_class(parser);
    if (byte == '.') return new_node(parser, REGEX_NODE_ANY, NULL, NULL, 0);
    if (byte == '^') return new_node(parser, REGEX_NODE_BOL, NULL, NULL, 0);
    if (byte == '$') return new_node(parser, REGEX_NODE_EOL, NULL, NULL, 0);
    if (byte == '\\') {
        uint32_t value = 0;
        uint32_t category = 0;
        if (!parse_escape(parser, &value, &category, false)) return NULL;
        if (category != 0) {
            const int32_t class_id = new_class(parser, value != 0);
            if (class_id < 0) return NULL;
            if (category & 1u) {
                (void)class_add_range(parser, (uint32_t)class_id, '0', '9');
            }
            if (category & 2u) {
                (void)class_add_range(parser, (uint32_t)class_id, '\t', '\r');
                (void)class_add_range(parser, (uint32_t)class_id, ' ', ' ');
            }
            if (category & 4u) {
                (void)class_add_range(parser, (uint32_t)class_id, '0', '9');
                (void)class_add_range(parser, (uint32_t)class_id, 'A', 'Z');
                (void)class_add_range(parser, (uint32_t)class_id, '_', '_');
                (void)class_add_range(parser, (uint32_t)class_id, 'a', 'z');
            }
            return new_node(parser, REGEX_NODE_CLASS, NULL, NULL,
                            (uint32_t)class_id);
        }
        return new_node(parser, REGEX_NODE_LITERAL, NULL, NULL, value);
    }
    if (byte == ')' || byte == '|' || byte == '*' || byte == '+'
        || byte == '?') {
        parser_fail(parser, REGEX_INVALID_PATTERN, atom_offset);
        return NULL;
    }
    --parser->cursor;
    uint32_t value = 0;
    if (!utf8_next(parser->pattern, parser->length, &parser->cursor, &value)) {
        parser_fail(parser, REGEX_INVALID_PATTERN, atom_offset);
        return NULL;
    }
    return new_node(parser, REGEX_NODE_LITERAL, NULL, NULL, value);
}

static Regex_Node *parse_repeat(Regex_Parser *parser, unsigned depth)
{
    Regex_Node *node = parse_atom(parser, depth);
    if (node == NULL) return NULL;
    if (parser->cursor < parser->length) {
        const uint8_t quantifier = parser->pattern[parser->cursor];
        Regex_Node_Kind kind;
        if (quantifier == '*') kind = REGEX_NODE_STAR;
        else if (quantifier == '+') kind = REGEX_NODE_PLUS;
        else if (quantifier == '?') kind = REGEX_NODE_OPTIONAL;
        else return node;
        ++parser->cursor;
        node = new_node(parser, kind, node, NULL, 0);
        if (parser->cursor < parser->length
            && (parser->pattern[parser->cursor] == '*'
                || parser->pattern[parser->cursor] == '+'
                || parser->pattern[parser->cursor] == '?')) {
            parser_fail(parser, REGEX_INVALID_PATTERN, parser->cursor);
            return NULL;
        }
    }
    return node;
}

static Regex_Node *parse_concat(Regex_Parser *parser, unsigned depth)
{
    Regex_Node *left = NULL;
    while (parser->cursor < parser->length
           && parser->pattern[parser->cursor] != ')'
           && parser->pattern[parser->cursor] != '|') {
        Regex_Node *right = parse_repeat(parser, depth);
        if (right == NULL) return NULL;
        if (left == NULL) {
            left = right;
        } else {
            left = new_node(parser, REGEX_NODE_CONCAT, left, right, 0);
        }
        if (parser->status != REGEX_OK) return NULL;
    }
    return left != NULL ? left
        : new_node(parser, REGEX_NODE_EMPTY, NULL, NULL, 0);
}

static Regex_Node *parse_expression(Regex_Parser *parser, unsigned depth)
{
    if (depth > REGEX_MAX_DEPTH) {
        parser_fail(parser, REGEX_RESOURCE_EXHAUSTED, parser->cursor);
        return NULL;
    }
    Regex_Node *left = parse_concat(parser, depth);
    while (left != NULL && parser->cursor < parser->length
           && parser->pattern[parser->cursor] == '|') {
        ++parser->cursor;
        Regex_Node *right = parse_concat(parser, depth);
        if (right == NULL) return NULL;
        left = new_node(parser, REGEX_NODE_ALTERNATE, left, right, 0);
    }
    return left;
}

static int32_t new_instruction(Regex_Compiler *compiler,
                               Regex_Instruction_Kind kind,
                               uint32_t value, int32_t out, int32_t out2)
{
    if (compiler->instruction_count == REGEX_MAX_INSTRUCTIONS) {
        compiler->status = REGEX_RESOURCE_EXHAUSTED;
        return -1;
    }
    const int32_t index = (int32_t)compiler->instruction_count++;
    compiler->instructions[index] = (Regex_Instruction){kind, out, out2, value};
    return index;
}

static int32_t new_patch(Regex_Compiler *compiler, int32_t instruction,
                         int32_t field)
{
    if (compiler->patch_count == REGEX_MAX_PATCHES) {
        compiler->status = REGEX_RESOURCE_EXHAUSTED;
        return -1;
    }
    const int32_t index = (int32_t)compiler->patch_count++;
    compiler->patches[index] = (Regex_Patch){instruction, field, -1};
    return index;
}

static Regex_Fragment singleton(Regex_Compiler *compiler,
                                Regex_Instruction_Kind kind, uint32_t value)
{
    const int32_t instruction = new_instruction(compiler, kind, value, -1, -1);
    const int32_t patch = instruction < 0 ? -1
        : new_patch(compiler, instruction, 1);
    return (Regex_Fragment){instruction, patch, patch};
}

static void patch_outs(Regex_Compiler *compiler, int32_t head,
                       int32_t destination)
{
    while (head >= 0) {
        Regex_Patch *patch = &compiler->patches[head];
        Regex_Instruction *instruction = &compiler->instructions[patch->instruction];
        if (patch->field == 1) instruction->out = destination;
        else instruction->out2 = destination;
        head = patch->next;
    }
}

static Regex_Fragment merge_outs(Regex_Compiler *compiler,
                                 Regex_Fragment left,
                                 Regex_Fragment right)
{
    (void)compiler;
    if (left.patch_head < 0) return right;
    if (right.patch_head < 0) return left;
    compiler->patches[left.patch_tail].next = right.patch_head;
    left.patch_tail = right.patch_tail;
    return left;
}

static Regex_Fragment compile_node(Regex_Compiler *compiler,
                                   const Regex_Parser *parser,
                                   const Regex_Node *node, unsigned depth)
{
    if (node == NULL || depth > REGEX_MAX_DEPTH) {
        compiler->status = REGEX_RESOURCE_EXHAUSTED;
        return (Regex_Fragment){-1, -1, -1};
    }
    Regex_Fragment left = {-1, -1, -1};
    Regex_Fragment right = {-1, -1, -1};
    int32_t instruction = -1;
    switch (node->kind) {
    case REGEX_NODE_EMPTY:
        return singleton(compiler, REGEX_INST_JUMP, 0);
    case REGEX_NODE_LITERAL:
        return singleton(compiler, REGEX_INST_CHAR, node->value);
    case REGEX_NODE_ANY:
        return singleton(compiler, REGEX_INST_ANY, 0);
    case REGEX_NODE_CLASS:
        return singleton(compiler, REGEX_INST_CLASS, node->value);
    case REGEX_NODE_BOL:
        return singleton(compiler, REGEX_INST_BOL, 0);
    case REGEX_NODE_EOL:
        return singleton(compiler, REGEX_INST_EOL, 0);
    case REGEX_NODE_CONCAT:
        left = compile_node(compiler, parser, node->left, depth + 1);
        right = compile_node(compiler, parser, node->right, depth + 1);
        patch_outs(compiler, left.patch_head, right.start);
        return (Regex_Fragment){left.start, right.patch_head, right.patch_tail};
    case REGEX_NODE_ALTERNATE:
        left = compile_node(compiler, parser, node->left, depth + 1);
        right = compile_node(compiler, parser, node->right, depth + 1);
        instruction = new_instruction(compiler, REGEX_INST_SPLIT, 0,
                                      left.start, right.start);
        {
            Regex_Fragment outs = merge_outs(compiler, left, right);
            return (Regex_Fragment){instruction, outs.patch_head, outs.patch_tail};
        }
    case REGEX_NODE_STAR:
        left = compile_node(compiler, parser, node->left, depth + 1);
        instruction = new_instruction(compiler, REGEX_INST_SPLIT, 0,
                                      left.start, -1);
        patch_outs(compiler, left.patch_head, instruction);
        {
            const int32_t patch = new_patch(compiler, instruction, 2);
            return (Regex_Fragment){instruction, patch, patch};
        }
    case REGEX_NODE_PLUS:
        left = compile_node(compiler, parser, node->left, depth + 1);
        instruction = new_instruction(compiler, REGEX_INST_SPLIT, 0,
                                      left.start, -1);
        patch_outs(compiler, left.patch_head, instruction);
        {
            const int32_t patch = new_patch(compiler, instruction, 2);
            return (Regex_Fragment){left.start, patch, patch};
        }
    case REGEX_NODE_OPTIONAL:
        left = compile_node(compiler, parser, node->left, depth + 1);
        instruction = new_instruction(compiler, REGEX_INST_SPLIT, 0,
                                      left.start, -1);
        {
            const int32_t patch = new_patch(compiler, instruction, 2);
            Regex_Fragment outs = merge_outs(
                compiler, left, (Regex_Fragment){instruction, patch, patch});
            return (Regex_Fragment){instruction, outs.patch_head, outs.patch_tail};
        }
    }
    compiler->status = REGEX_INVALID_PATTERN;
    return (Regex_Fragment){-1, -1, -1};
}

static bool class_matches(const Regex_Parser *parser, uint32_t class_id,
                          uint32_t value, size_t *step_count)
{
    if (class_id >= parser->class_count) return false;
    const Regex_Class *character_class = &parser->classes[class_id];
    bool found = false;
    for (size_t index = 0; index < character_class->range_count; ++index) {
        if (++(*step_count) > REGEX_STEP_LIMIT) return false;
        const Regex_Range *range = &parser->ranges[
            (size_t)character_class->first_range + index];
        if (value >= range->first && value <= range->last) {
            found = true;
            break;
        }
    }
    return character_class->negate ? !found : found;
}

static bool add_closure(const Regex_Compiler *compiler,
                        int32_t *starts, int32_t *queue,
                        size_t queue_capacity, int32_t start,
                        int32_t origin, size_t position, size_t input_length,
                        size_t *step_count)
{
    size_t read_index = 0;
    size_t write_index = 0;
    if (start < 0) return true;
    if (starts[start] <= origin) return true;
    starts[start] = origin;
    queue[write_index++] = start;
    while (read_index < write_index) {
        if (++(*step_count) > REGEX_STEP_LIMIT) return false;
        const int32_t index = queue[read_index++];
        const Regex_Instruction *instruction = &compiler->instructions[index];
        int32_t next_a = -1;
        int32_t next_b = -1;
        switch (instruction->kind) {
        case REGEX_INST_JUMP:
            next_a = instruction->out;
            break;
        case REGEX_INST_SPLIT:
            next_a = instruction->out;
            next_b = instruction->out2;
            break;
        case REGEX_INST_BOL:
            if (position == 0) next_a = instruction->out;
            break;
        case REGEX_INST_EOL:
            if (position == input_length) next_a = instruction->out;
            break;
        default:
            break;
        }
        const int32_t destinations[2] = {next_a, next_b};
        for (size_t slot = 0; slot < 2; ++slot) {
            const int32_t destination = destinations[slot];
            if (destination < 0 || starts[destination] <= origin) continue;
            if (write_index == queue_capacity) return false;
            starts[destination] = origin;
            queue[write_index++] = destination;
        }
    }
    return true;
}

static char *format_result(int status, size_t offset,
                           bool matched, size_t start, size_t end)
{
    char buffer[96];
    int length = 0;
    if (status == REGEX_OK) {
        length = snprintf(buffer, sizeof(buffer), "ok|%u|%u|%u",
            matched ? 1u : 0u, matched ? (unsigned)start : 0u,
            matched ? (unsigned)end : 0u);
    } else {
        const char *code = status == REGEX_UNSUPPORTED ? "unsupported"
            : status == REGEX_INVALID_UTF8 ? "invalid_utf8"
            : status == REGEX_RESOURCE_EXHAUSTED ? "resource_exhausted"
            : "invalid_pattern";
        length = snprintf(buffer, sizeof(buffer), "error|%s|%u", code,
                          (unsigned)offset);
    }
    if (length < 0 || (size_t)length >= sizeof(buffer)) return NULL;
    char *result = om_alloc((size_t)length + 1);
    if (result == NULL) return NULL;
    memcpy(result, buffer, (size_t)length + 1);
    return result;
}

static char *regex_run(const char *pattern, const char *input)
{
    if (pattern == NULL || input == NULL) {
        return format_result(REGEX_INVALID_PATTERN, 0, false, 0, 0);
    }
    const size_t pattern_length = strlen(pattern);
    const size_t input_length = strlen(input);
    if ((uint64_t)pattern_length > REGEX_MAX_PATTERN_BYTES
        || (uint64_t)input_length > REGEX_MAX_TEXT_BYTES) {
        return format_result(REGEX_RESOURCE_EXHAUSTED, 0, false, 0, 0);
    }
    Regex_Parser parser = {0};
    parser.pattern = (const uint8_t *)pattern;
    parser.length = (size_t)pattern_length;
    Regex_Node *root = parse_expression(&parser, 0);
    if (parser.status == REGEX_OK && parser.cursor != parser.length) {
        parser_fail(&parser, REGEX_INVALID_PATTERN, parser.cursor);
    }
    if (parser.status != REGEX_OK || root == NULL) {
        const int status = parser.status == REGEX_OK
            ? REGEX_INVALID_PATTERN : parser.status;
        for (size_t index = 0; index < parser.node_count; ++index) {
            om_free(parser.nodes[index]);
        }
        return format_result(status, parser.error_offset, false, 0, 0);
    }
    Regex_Compiler compiler = {0};
    Regex_Fragment fragment = compile_node(&compiler, &parser, root, 0);
    const int32_t accept = new_instruction(&compiler, REGEX_INST_MATCH, 0, -1, -1);
    if (compiler.status == REGEX_OK) patch_outs(&compiler, fragment.patch_head, accept);
    if (compiler.status != REGEX_OK || fragment.start < 0) {
        for (size_t index = 0; index < parser.node_count; ++index) {
            om_free(parser.nodes[index]);
        }
        return format_result(compiler.status == REGEX_OK
            ? REGEX_RESOURCE_EXHAUSTED : compiler.status,
            parser.length, false, 0, 0);
    }

    const size_t input_bytes = (size_t)input_length;
    size_t maximum_scalars = input_bytes;
    uint32_t *scalars = maximum_scalars == 0 ? NULL
        : om_alloc_array(maximum_scalars, sizeof(*scalars));
    int32_t *current = om_alloc_array(compiler.instruction_count,
                                      sizeof(*current));
    int32_t *next = om_alloc_array(compiler.instruction_count, sizeof(*next));
    int32_t *queue = om_alloc_array(compiler.instruction_count,
                                    sizeof(*queue));
    if ((maximum_scalars != 0 && scalars == NULL) || current == NULL
        || next == NULL || queue == NULL) {
        om_free(scalars);
        om_free(current);
        om_free(next);
        om_free(queue);
        for (size_t index = 0; index < parser.node_count; ++index) {
            om_free(parser.nodes[index]);
        }
        return format_result(REGEX_RESOURCE_EXHAUSTED, 0, false, 0, 0);
    }
    size_t scalar_count = 0;
    size_t byte_cursor = 0;
    while (byte_cursor < input_bytes) {
        const size_t scalar_offset = byte_cursor;
        if (!utf8_next((const uint8_t *)input, input_bytes,
                       &byte_cursor, &scalars[scalar_count])) {
            om_free(scalars);
            om_free(current);
            om_free(next);
            om_free(queue);
            for (size_t index = 0; index < parser.node_count; ++index) {
                om_free(parser.nodes[index]);
            }
            return format_result(REGEX_INVALID_UTF8, scalar_offset,
                                 false, 0, 0);
        }
        ++scalar_count;
    }
    const size_t instruction_count = compiler.instruction_count;
    const int32_t infinity = INT32_MAX;
    for (size_t index = 0; index < instruction_count; ++index) {
        current[index] = infinity;
        next[index] = infinity;
    }
    size_t steps = 0;
    bool matched = false;
    size_t match_start = 0;
    size_t match_end = 0;
    bool exhausted = false;
    for (size_t position = 0; position <= scalar_count; ++position) {
        if (!add_closure(&compiler, current, queue, instruction_count,
                         fragment.start, (int32_t)position, position,
                         scalar_count, &steps)) {
            exhausted = true;
            break;
        }
        int32_t best_here = infinity;
        int32_t earliest_active = infinity;
        for (size_t index = 0; index < instruction_count; ++index) {
            if (++steps > REGEX_STEP_LIMIT) {
                exhausted = true;
                break;
            }
            if (current[index] == infinity) continue;
            const Regex_Instruction *instruction = &compiler.instructions[index];
            if (instruction->kind == REGEX_INST_MATCH) {
                if (current[index] < best_here) best_here = current[index];
            } else if (instruction->kind == REGEX_INST_CHAR
                       || instruction->kind == REGEX_INST_ANY
                       || instruction->kind == REGEX_INST_CLASS) {
                if (current[index] < earliest_active) {
                    earliest_active = current[index];
                }
            }
        }
        if (exhausted) break;
        if (best_here != infinity) {
            if (!matched || (size_t)best_here < match_start) {
                matched = true;
                match_start = (size_t)best_here;
                match_end = position;
            }
        }
        if (matched && earliest_active >= (int32_t)match_start) break;
        if (position == scalar_count) break;
        for (size_t index = 0; index < instruction_count; ++index) {
            if (++steps > REGEX_STEP_LIMIT) {
                exhausted = true;
                break;
            }
            next[index] = infinity;
        }
        if (exhausted) break;
        const uint32_t scalar = scalars[position];
        for (size_t index = 0; index < instruction_count; ++index) {
            if (++steps > REGEX_STEP_LIMIT) {
                exhausted = true;
                break;
            }
            const int32_t origin = current[index];
            if (origin == infinity) continue;
            const Regex_Instruction *instruction = &compiler.instructions[index];
            bool accepts = false;
            if (instruction->kind == REGEX_INST_CHAR) {
                accepts = scalar == instruction->value;
            } else if (instruction->kind == REGEX_INST_ANY) {
                accepts = scalar != '\n';
            } else if (instruction->kind == REGEX_INST_CLASS) {
                accepts = class_matches(&parser, instruction->value, scalar,
                                        &steps);
                if (steps > REGEX_STEP_LIMIT) {
                    exhausted = true;
                    break;
                }
            }
            if (accepts && !add_closure(&compiler, next, queue,
                    instruction_count, instruction->out, origin,
                    position + 1, scalar_count, &steps)) {
                exhausted = true;
                break;
            }
        }
        if (exhausted) break;
        int32_t *swap = current;
        current = next;
        next = swap;
    }

    om_free(scalars);
    om_free(current);
    om_free(next);
    om_free(queue);
    for (size_t index = 0; index < parser.node_count; ++index) {
        om_free(parser.nodes[index]);
    }
    if (exhausted) {
        return format_result(REGEX_RESOURCE_EXHAUSTED, 0, false, 0, 0);
    }
    return format_result(REGEX_OK, 0, matched, match_start, match_end);
}

char *om_runtime_regex_find(const char *pattern, const char *input)
{
    return regex_run(pattern, input);
}

void om_runtime_regex_release(void *value)
{
    om_free(value);
}
