# Syntax capability

The Syntax capability depends on Source and provides the OMBasic 0.1 lexer, syntax-only AST, and parser.

`om_lexer_scan` returns source-backed token lexemes, stable lexical diagnostic codes, parsed integer magnitudes, and locale-independent binary64 values. Interpolated strings are split into start, text, interpolation-boundary, expression, and end tokens; text lexemes remain raw so later syntax stages can decode escapes and doubled braces without losing source spans.

The lexer preserves LF and CRLF as `NEWLINE` tokens outside parenthesized, bracketed, braced, and interpolation expressions. Ordinary comments are discarded; documentation comments remain tokens for later declaration association. Invalid UTF-8, unsupported source characters, malformed escapes, malformed numbers, and keyword casing errors produce diagnostics and scanning continues where a safe boundary is known.

Integer magnitude `2^63` is retained without E0104 and marked `integer_requires_negative`; the parser must accept it only as the direct operand of unary minus to represent `INT64_MIN`. Larger magnitudes are diagnosed by the lexer.

`om_parser_parse` consumes the token slice and returns an immutable node/child store with borrowed source views. Declarations and statements use recursive descent; expressions use Pratt precedence. The parser covers imports, declarations, Actor state and receives, control flow, calls, collections, interpolation, `TRY`, `SPAWN`, and `WITH`. Parser diagnostics E0202–E0208 preserve source spans. Recovery advances to line boundaries and enclosing block terminators, and expression/block nesting is capped to bound stack use. Child ordering and node flags are documented in `ast.h`.

The parser tests compare normalized output with `tests/conformance/parser/basic.ast`, exercise the language grammar, assert lexical and parser spans, run 4,096 deterministic malformed inputs, and verify both nesting limits.
