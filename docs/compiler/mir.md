# Native MIR

MIR is the backend contract between specialization and LLVM. It is a typed
control-flow graph, not a stack machine.

## Function and block model

Each function has a concrete signature, an entry block, typed values, basic
blocks, block parameters, and exactly one terminator per block. Branch
arguments match the target block parameters; LLVM lowering turns block
parameters into PHI nodes. Definitions dominate their uses.

## Operation families

- Scalar constants, checked integer arithmetic, floating-point arithmetic,
  and comparisons.
- Object allocation, field loads/initialization, managed copies, and
  descriptor-driven List/Map operations.
- Concrete Result construction, tag tests, and payload access.
- Runtime intrinsics for faults, strings, Actors, system services, and
  callbacks.
- Explicit Native calls and CString conversions.
- Branch, conditional branch, switch, return, and unreachable terminators.

There are no instruction pointers, opcode dispatch, operand stacks, or VM
frames. Generic values must be specialized before reaching MIR.

## Verification

The MIR verifier checks terminators, definition/use validity, dominance,
branch argument types, concrete function signatures, runtime intrinsic
effects, Native blocking restrictions in Actor handlers, and absence of
unresolved specialization. MIR APIs and tests are implemented with Phase 5.

The construction API, deterministic dump, and verifier for CFG shape,
concrete values, call signatures, block arguments, dominance, and effect rules
are implemented. HIR-to-MIR lowers concrete Function/Action instances,
direct and function-valued indirect calls, assignments, returns, `IF`,
`WHILE`, numeric `FOR` loops,
including signed steps and the zero-step fault edge, and String literals and
interpolation over Int, Float, Bool, String, and Null values. String bytes are
owned by the MIR project and escaped in deterministic dumps. List/Map literals,
indexing, and `FOR EACH` loops lower to typed runtime operations; Map iteration
produces keys in insertion order. Record/Message construction, field reads,
Record updates, `Result`/`TRY`, `SELECT`, and Null constants lower to MIR.
Record/Message descriptors let the verifier check constructor arity and field
ownership/types. MIR now lowers direct Native calls with scalar and CString
metadata; the LLVM backend materializes CString temporaries at the ABI
boundary. Native Records use target-aware aggregate ABI lowering, and managed
`CPointer` values use the Runtime ownership API. Generated callback
trampolines copy supported scalar payloads into Runtime mailbox ingress; they
do not execute Actor handlers on foreign threads. The Native library must stop
and join callback threads before its call returns.
LLVM types and values remain private to
`src/backend/llvm/`; MIR types do not expose LLVM handles.
