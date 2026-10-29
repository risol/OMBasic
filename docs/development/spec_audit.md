# OMBasic 0.1 implementation audit

Status: Phase 0 specification audit is complete. The implementation bindings below are normative in Language Standard §37 and ADRs 0001–0006; the lexer and parser may use them as their contract.

## Decisions

| Topic | Implementation binding |
|---|---|
| Reserved words | Spellings listed in Language Standard 5.7 are reserved case-insensitively. Only exact uppercase spellings are keyword tokens; other casing reports E0101. |
| Ordinary and entity identifiers | Ordinary names use ASCII lower_snake_case; entity names use Pascal_Snake_Case. Invalid mixed-case spellings are rejected rather than rewritten. |
| Module contents | A source file contains imports first, followed by declarations. Executable module-level statements are forbidden, matching the no-initialization rule. |
| Integer literals | Decimal digits only; no digit separators. The signed range is Int64. An out-of-range positive literal is a compile diagnostic, except the spelling used as the operand of unary minus may represent INT64_MIN. |
| Integer operations | Add, subtract, multiply, negate, divide, and remainder use checked Int64 semantics. Overflow and division by zero are runtime errors. Integer division truncates toward zero. |
| Float literals and operations | Decimal literals may use an exponent and must round to finite IEEE-754 binary64. Literal overflow is a compile diagnostic. Arithmetic follows binary64; comparisons follow IEEE-754, including NaN behavior. |
| Addition | `+` accepts two Ints, two Floats, or two Strings. String addition concatenates valid UTF-8 bytes into a managed String; mixed operand types are rejected. |
| String comparison | Equality and ordering use Unicode scalar value order. UTF-8 byte lexicographic order preserves scalar order for valid UTF-8. |
| Native imports | `NATIVE IMPORT "path.omi"` makes ABI declarations visible only in the importing `.bas` Module. Native metadata is not re-exported and does not enter the source Module Graph. |
| String escapes | String literals are UTF-8 and support \\, \", \n, \r, \t, \0, and \u{hex}. Invalid Unicode scalar values and malformed UTF-8 are rejected. |
| Interpolation | Interpolated strings use $"..."; a single balanced {...} contains an expression. {{ and }} encode literal braces. Interpolation is restricted to deterministic scalar formatting: Int, Float, Bool, String, and Null. |
| Null | Null is a distinct singleton type in 0.1. There is no nullable/union type syntax, so Null cannot be assigned to a non-Null type. |
| Shadowing | A declaration cannot reuse any local name visible through its enclosing lexical scopes. Sibling scopes may reuse names. Imported module qualifiers and prelude names cannot be shadowed. |
| Result constructors | Result is a built-in tagged union. The built-in constructors are Ok(value) and Error(error); their type parameters are inferred from context. |
| TRY | TRY accepts Result<T,E>. Its enclosing return channel must be Result<...,E>. In a Receive handler, propagating Error terminates that Actor as failed; watchers receive Actor_Down with reason `Actor_Down_Reason.failed`. Action and Function propagate Error through their inferred Result return. |
| SELECT | The scrutinee is evaluated once. CASE expressions are evaluated in order and use built-in equality; there is no fallthrough. CASE ELSE is optional, unique, and last. Result cases may destructure Ok(name) or Error(name). |
| WITH | Record update copies the value and replaces one or more distinct, known fields. Each replacement must have the declared field type. |
| SPAWN and AT | Actor constructors take no arguments in 0.1; actor state initializers run once per instance and must be pure. SPAWN Actor creates locally; SPAWN Actor AT node requires a Node_Ref. |
| WATCH | WATCH is valid only while executing an Actor. It registers a one-shot watch for the current Actor. A predefined Actor_Down(actor_id, reason) system Message reports termination; reason is an `Actor_Down_Reason` value. `Actor_Down_Reason.node_unreachable` means the remote Actor's state could not be confirmed, not that termination was proven. Repeating the same watch is idempotent. |
| Native metadata | .omi has its own grammar. Every Native function declares ABI parameter and result types, an effect, and blocking status. Pointer ownership is explicit; Owned returns name a release function, and Borrowed returns name their owner parameter. |
| Entry point | The selected root Module must define exactly one ACTION main() with no parameters and Unit result. Imported modules cannot provide the entry point. |
| Source root and imports | Imports are Project-root-relative, module-relative `./`, or explicit SDK `@std/`. Root kind plus canonical relative `.bas` path is the module identity. Absolute paths, empty segments, parent segments, aliases, wrong case, and symlink/reparse traversal are rejected; there is no fallback. The graph is acyclic. |
| Collections | List and Map literals are immutable values. Empty literals require an expected type. Map keys are limited to Int, Bool, and String in 0.1; duplicate literal keys are rejected. |
| Diagnostics | Invalid user source always produces a diagnostic, never an assertion failure. Diagnostics carry a source span when one exists. |
| Identifier diagnostics | E0102 reports an identifier that matches neither ordinary nor entity naming form. Diagnostic spans use one-based lines and one-based UTF-8 byte columns with an exclusive end. |
| Literal diagnostics | E0103 reports malformed lexical tokens; E0104 reports integer or Float literals outside the supported value range. |

## Grammar audit resolution

The initial standard left the following grammar questions open. Section 37 now resolves them normatively:

- token boundaries, comments, newline handling, malformed UTF-8, and lexical errors (§37.1);
- numeric, string, escape, and interpolation forms (§37.1);
- expression operators, precedence, calls, member access, collections, Record updates, and Result patterns (§37.2);
- binding versus reassignment, assignment targets, statement termination, declarations, import order, and Actor blocks (§37.2);
- control flow and Actor statements (§37.2–§37.3);
- the separate .omi token mode, grammar, ABI spellings, and annotation cardinality (§37.4).

The standard is the single normative source. Diagnostic codes and byte-span conventions are recorded in diagnostics.md and ADR-0006.

## Gate evidence

Phase 0 is complete because:

1. the standard contains the bindings above and lexical/core/.omi grammar;
2. the conformance manifest contains keyword and naming positive/negative fixtures with stable diagnostic spans;
3. ADR-0001 through ADR-0006 exist;
4. implementation-specific choices are recorded in the standard and architecture docs, including source-tree resolution in ADR-0007.
