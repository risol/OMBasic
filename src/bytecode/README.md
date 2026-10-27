# Bytecode capability

The Bytecode capability consumes syntax, name-resolution, type-inference, and
semantic-analysis results. It builds an owned Om_Bytecode_Image, verifies it,
and reads or writes the portable OMBC format.

## OMBC version 3

Every integer field is little-endian. The file does not contain C structures,
padding, pointers, or native enum representations.

The 56-byte header is:

| Offset | Field |
| --- | --- |
| 0 | Four bytes OMBC |
| 4 | u16 format version (3) |
| 6 | u16 reserved (0) |
| 8 | u32 constant count |
| 12 | u32 function count |
| 16 | u32 instruction count |
| 20 | u32 source-map entry count |
| 24 | u32 Actor count |
| 28 | u32 Actor handler count |
| 32 | u32 Record/Message schema count |
| 36 | u32 schema-field count |
| 40 | u64 program fingerprint |
| 48 | u64 interface fingerprint |

Sections follow in that order: constants, functions, instructions, source-map
entries, Actors, Actor handlers, schemas, then schema fields.

Constant tags are 0 Null, 1 Unit, 2 Bool, 3 signed 64-bit integer,
4 IEEE-754 binary64, 5 Down_Reason, and 6 UTF-8 String. Variable-length
strings use a u32 byte length followed by exactly that many bytes.

Function records contain seven u32 fields: name constant, entry PC, end PC,
parameter count, local count, maximum stack depth, and flags. An external
function has an empty code range and zero maximum stack. Variadic is allowed
only for external functions.

An instruction is exactly 17 bytes: u8 opcode, u32 operand A, u32 operand B,
and a reserved signed i64 immediate which must currently be zero. Jump targets
are absolute instruction indexes. Function ranges and source-map ranges are
half-open.

Actor records point to their initializer and a contiguous range in the handler
table. Schemas identify a Record or Message symbol, a contiguous range of
field-name constants, and a stable 64-bit schema fingerprint. Each schema
record is four u32 fields followed by a u64 fingerprint. Constructors carry the
schema symbol and positional field count, so a loader can reject missing or
extra fields before execution. Record and Message fingerprints are derived
from the canonical module path, entity name, and ordered structural field-type
fingerprints. The program fingerprint hashes canonical source module paths and
contents; the interface fingerprint hashes the ordered schema fingerprints.

## Operand definitions

Operands not listed for an instruction must be zero.

| Instruction | Operand A | Operand B | Stack effect |
| --- | --- | --- | --- |
| LOAD_CONST | constant index | — | 0 → 1 |
| LOAD_LOCAL, STORE_LOCAL | local slot | — | load 0 → 1; store 1 → 0 |
| JUMP, JUMP_IF_FALSE, JUMP_IF_TRUE | absolute PC | — | jump 0 → 0; conditional 1 → 0 |
| CALL | function index | argument count | n → 1 |
| CALL_INDIRECT | — | argument count | callable and arguments n+1 → 1 |
| LOAD_FUNCTION | function index | — | 0 → 1 |
| LOAD_SYSTEM_ACTOR | built-in Actor enum | — | 0 → 1 |
| MAKE_RECORD, MAKE_MESSAGE | schema symbol | field count | n → 1 |
| GET_FIELD, WITH_FIELD | string constant | — | get 1 → 1; update 2 → 1 |
| MAKE_LIST, MAKE_MAP | item count / pair count | — | n → 1 / 2n → 1 |
| ITER_NEXT | iterator local | destination local | stores an item and pushes a Bool |
| SPAWN | Actor symbol | placement-present flag (0 or 1) | optional placement n → 1 |
| LOAD_STATE, STORE_STATE | Actor state slot | — | load 0 → 1; store 1 → 0 |

Arithmetic, comparisons, Result, string, Actor, and control-flow opcodes have
zero operands unless listed above. CHECK_RESULT preserves a Result and pushes a
Bool. UNWRAP_OK and UNWRAP_ERROR replace a Result with its payload.
RETURN_ERROR terminates the current function with the Error payload.
FOR_RANGE_TEST consumes current, limit, and step and pushes whether an
inclusive numeric range has another item.

## Verification and emission

om_bytecode_decode enforces the configured byte limit (64 MiB by default),
checks section lengths before allocating, rejects unknown versions and trailing
bytes, and runs the verifier before exposing an image. The verifier checks
constant and schema indexes, function ownership/ranges, Actor protocol tables,
legal operands, control-flow targets, local slots, source maps, and exact stack
height agreement at control-flow joins. It derives maximum stack depth while
finishing a builder image.

The emitter lowers expressions and statements directly from the AST and
semantic results. It includes short-circuit AND/OR, numeric and collection
FOR, TRY propagation, SELECT literal and Ok/Error patterns, Actor state
initialization and receive handlers, and source spans. Unsupported AST nodes
produce an emitter diagnostic and no image.
