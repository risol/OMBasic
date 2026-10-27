# ADR-0008: Use level-based Hindley–Milner type schemes

Status: Accepted

## Context

OMBasic has no ordinary source-level type annotations, but functions can be
polymorphic. Types also need stable IDs because semantic results outlive Type
Store vector growth. Structural types and nominal declarations need different
compatibility rules, and recursive types must be rejected.

## Decision

- Keep type facts in a Type Store, separate from the immutable AST.
- Intern primitive, structural, and nominal type nodes and refer to them only
  by `Type_Id`.
- Represent inference variables as unique nodes with a level and optional
  instance. Unification is structural for List, Map, Result, Function, and
  Action; Record, Message, and Actor protocol identity uses `Symbol_Id`.
- Use an occurs check. A failed structural unification rolls back every type
  variable link or level change made by that attempt.
- Generalize unbound variables above the environment level into a type scheme;
  instantiate each quantified variable with a fresh TypeVar at each use.
- Preserve Numeric, Integral, Comparable, Ordered, Printable, HashKey,
  ActorRef, and Message requirements on quantified variables. A later binding
  must satisfy every inherited requirement.
- Permit ActorRef to contain an unresolved Actor protocol TypeVar while
  inferring generic send/watch code; unification later ties it to a nominal
  Actor declaration.
- Keep Int, Float, and Null distinct. Ordinary OMBasic code has no implicit
  numeric conversion and no user-visible `Any` type.

## Consequences

- Generic functions can retain stable public schemes such as `T -> T`.
- Nominally different Record, Message, or Actor declarations never unify just
  because their fields happen to match.
- Diagnostics and editor tooling can query types without modifying AST nodes.
- Type Store handles and schemes have explicit lifetimes; the store owns type
  nodes, while a scheme owns only its quantified-variable list.
