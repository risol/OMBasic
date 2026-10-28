# Typed HIR

HIR is the typed, resolved boundary between frontend semantic analysis and
backend-independent native compilation. It owns OMBasic meaning; LLVM does
not.

## Entities and identity

The initial model contains modules, functions, Actions, Records, Messages,
Actors, receives, parameters, and local bindings. Each entity has a stable
typed ID, source span, canonical name, and owning module. HIR references only
resolved `Symbol_Id`, nominal `Type_Id`, and explicit protocol/schema IDs.
Unresolved names and parser-only syntax nodes do not cross this boundary.

## Values and operations

Expressions cover literals, local reads, direct/indirect calls, field reads,
Record/Message construction, Record updates, List/Map operations, unary and
binary operations, explicit `Result.Ok`/`Result.Error` construction and `TRY`,
Actor spawn/send/watch, and system Actor references. Statements cover bindings, mutable assignment,
Actor-state assignment, conditionals, loops, selection, returns, stop, and
call statements.

Each expression carries its inferred concrete or still-specializable type,
source span, and semantic effect summary. Actor operations also carry the
Actor protocol and Message schema identity. Values use typed IDs and owned
arena storage; the representation is not a bytecode instruction stream.

## Lowering and verification

Lowering consumes syntax, name-resolution, type-inference, and semantic side
tables. The same source and semantic model must produce deterministic HIR.
The project borrows those semantic indexes for as long as HIR remains alive.
The verifier checks module/entity ranges, single-parent tree structure, source
spans, symbol and type references, function effects, Actor-state placement,
`TRY` inputs, and `SEND` protocol and Message serializability. Golden dumps cover
single-module and multi-module programs, system Actor messages, and calls
resolved through Native metadata.

This HIR is a structured tree. Basic blocks and terminators belong to MIR and
are verified there; they are not represented by HIR.

HIR never includes LLVM headers. Storage, AST lowering, deterministic dumps,
golden normalized dumps, and the semantic verifier are implemented in Phase 3.
Generic instance discovery is documented separately in
[specialization.md](specialization.md).
