# Testing strategy

Tests are organized by capability and expand with the implementation:

1. Unit tests cover Foundation algorithms and boundary behavior.
2. Syntax conformance tests cover tokens, ASTs, and malformed-source recovery.
3. Semantic and compile-fail tests cover names, types, effects, protocols, and serialization.
4. Bytecode tests cover deterministic emission, verifier rejection, and VM instructions.
5. Runtime tests cover values, GC, actors, mailboxes, and scheduling.
6. Integration tests compile .bas inputs and compare diagnostics or output.
7. Distribution tests use two local nodes and malformed payloads.

The Lexer suite also runs 4,096 deterministic arbitrary-byte inputs, checks token/diagnostic span bounds, and runs the identifier/keyword conformance fixtures through the scanner. The Parser suite compares normalized AST output with golden files, checks recovery and diagnostic spans, and runs 4,096 deterministic malformed inputs plus expression and block nesting-limit cases.

CTest is the local test entry point. A test must include invalid-input cases when its capability accepts external data. Sanitizer profiles are separate build configurations. Fuzz targets are added when each parser/decoder capability exists.
