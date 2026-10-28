# ADR-0018: Use level-based Hindley–Milner type schemes

Status: Accepted

## Context

OMBasic has no ordinary source-level type annotations, but functions can be
polymorphic. Types need stable IDs because semantic results outlive Type Store
vector growth. Structural types and nominal declarations need different
compatibility rules, and recursive types must be rejected.

## Decision

- Keep type facts in a Type Store separate from the immutable AST. Intern
  primitive, structural, and nominal type nodes and refer to them by `Type_Id`.
- Represent inference variables as unique nodes with a level and optional
  instance. Unification is structural for List, Map, Result, Function, and
  Action; Record, Message, and Actor protocol identity uses `Symbol_Id`.
- Use an occurs check. A failed structural unification rolls back every type
  variable link or level change made by that attempt.
- Generalize unbound variables above the environment level into a type scheme
  and instantiate quantified variables freshly at each use.
- Preserve Numeric, Integral, Comparable, Ordered, Printable, HashKey,
  ActorRef, and Message requirements on quantified variables.
- Keep Int, Float, and Null distinct; ordinary source code has no implicit
  numeric conversion or user-visible `Any` type.

## Consequences

- Generic functions retain stable schemes such as `T -> T`.
- Nominal Record, Message, and Actor declarations never unify only because
  their fields happen to match.
- Semantic clients can query type facts without mutating syntax nodes.
