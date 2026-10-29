# Diagnostic conventions

Diagnostics are part of the compiler's public behavior. Invalid source must produce a stable diagnostic rather than an assertion or crash.

## Source spans

- Lines and columns are one-based.
- Columns count UTF-8 bytes, not Unicode scalar values.
- Start positions are inclusive and end positions are exclusive.
- A span belongs to one source file and never crosses a line terminator.
- Diagnostics caused by malformed UTF-8 use the byte range of the malformed sequence when it can be identified.

## Initial stable codes

| Code | Meaning |
|---|---|
| E0101 | A reserved keyword spelling uses casing other than its exact uppercase spelling. |
| E0102 | An identifier matches neither the ordinary-name nor entity-name pattern. |
| E0103 | A malformed or unsupported lexical token, including malformed UTF-8 or an invalid escape. |
| E0104 | A numeric literal is outside its representable Int64 or finite binary64 range. |

| E0201 | An import path is invalid for the configured source root. Reserved by the development plan for import validation. |
| E0202 | The parser expected a different token or found malformed syntax. |
| E0203 | A module contains an unsupported top-level construct or an import after declarations. |
| E0204 | An expression was required at this position. |
| E0205 | The `2^63` integer magnitude is not the direct operand of unary minus. |
| E0206 | A comparison expression chains more than one comparison operator. |
| E0207 | A standalone expression statement is not a call. |
| E0208 | Parsing hit its recovery or nesting safety limit. |
| E0301 | A name or qualified module member cannot be resolved. |
| E0302 | An imported module member exists but is private. |
| E0303 | A declaration repeats a name in the same scope. |
| E0304 | A declaration conflicts with a visible name using different casing. |
| E0305 | A declaration shadows a name visible from an enclosing scope or the prelude. |
| E0306 | An imported module alias is invalid or conflicts with another declaration. |
| E0307 | An assignment targets an immutable binding. |
| E0401 | An expression, argument, field, return value, or exported field has an incompatible or unstable type. |
| E0402 | An empty List or Map has no contextual type from which to infer its element, key, or value type. |
| E0403 | A Native callback binding has no unique Message accepted by the current Actor, has a field-count mismatch, or is registered outside an Actor receive handler. |
| E0404 | A Result expression has a type parameter that cannot be inferred from its context. |
| E0501 | A Function or Actor state initializer has a forbidden effect, or WATCH/STOP appears outside an Actor receive handler. |
| E0601 | A SEND target has an unknown Actor protocol or does not accept the Message. |
| E0701 | A Message payload contains a value that cannot be serialized. |

Code ranges are grouped by compiler stage. A code is never reused for a different meaning. Wording may improve while code and primary-span behavior remain stable.

## Conformance expectations

Compile-fail fixtures store the expected code and primary span in tests/conformance/manifest.json. Tests may add secondary spans and notes as the relevant capability is implemented; those do not replace the primary location.
