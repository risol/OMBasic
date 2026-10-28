# Generic specialization

Specialization turns reachable generic HIR functions into concrete function
instances before MIR lowering. An instance is keyed by its HIR symbol and its
canonical, concrete type arguments. Type constructors in the inference store
are reused so equivalent argument lists do not create duplicate instances.

## Reachability

The current worklist starts from Actions, Actor receive handlers, and Actor
state initializers. It follows direct HIR calls, specializes their argument
types in the caller's instance, and adds newly discovered instances to the
worklist. Receive handlers and state initializers have no generic arguments.
It also exposes a deterministic, deduplicated list of concrete reachable
List, Map, and Result type IDs for later static descriptor emission.

## Diagnostics

The API returns `OM_STATUS_OK` when it successfully builds a specialization
result, including when that result contains compile diagnostics. Callers must
inspect `diagnostic_count` before lowering the instances further.

- `E1001`: a function is reached recursively with changing type arguments,
  which would cause polymorphic recursion and unbounded specialization.
- `E1002`: a generic argument remains unresolved or violates a declared type
  constraint at a call site.
- `E1003`: the instance or type-argument safety limit was reached.

Diagnostics carry the HIR module, call node, and symbol IDs. The compiler
pipeline runs specialization after HIR verification and before MIR lowering,
and presents these errors through the normal diagnostic path.

## Implemented pipeline

The implementation canonicalizes concrete argument types, deduplicates
instances, traverses recursive calls, bounds total work, and reports
specialization diagnostics through the compiler pipeline. The reachable
concrete type IDs drive static descriptor emission. Generic container
operations pass those descriptors to shared Runtime helpers.
