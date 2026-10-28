# ADR-0006: Define explicit HIR and MIR boundaries

Status: Accepted

## Context

The compiler needs a stable boundary between source-language meaning and
machine-code generation. Letting LLVM consume AST details would make language
semantics target-specific, while a stack-machine IR would retain the former
VM execution model.

## Decision

- Lower resolved, typed source programs into HIR that owns language-level
  entities, operations, source locations, effects, and Actor protocol facts.
- Keep HIR structured. It may retain specializable types but contains no
  LLVM handles or bytecode operations.
- Specialize reachable generic functions before MIR construction.
- Lower concrete programs into MIR as typed functions, values, basic blocks,
  block parameters, and explicit terminators.
- Verify HIR and MIR at their boundaries. MIR verification checks concrete
  signatures, control-flow structure, definitions and uses, dominance, edge
  arguments, operation types, and effect restrictions.
- Keep LLVM-specific types and values private to the LLVM backend.

## Consequences

- Language semantics and CFG correctness can be tested without LLVM.
- The LLVM backend consumes verified concrete MIR rather than parsing source
  or deciding language behavior.
- New language operations must be represented and verified before they can be
  emitted as machine code.
