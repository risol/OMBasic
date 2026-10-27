# Conformance fixtures

Add language-standard cases under a directory named by its standard section, for example 05_identifiers or 16_actor. Each case records:

- a stable case ID containing the standard section;
- a .bas input;
- whether compilation should succeed;
- the expected diagnostic code and span for failures.

The compiler test runner will consume these fixtures after the lexer and parser are available. Foundation tests run earlier through CTest.

The checked-in manifest is manifest.json. Version 1 records the standard section, source path relative to this directory, acceptance result, and (for failures) a stable diagnostic code plus primary span. Span lines and UTF-8 byte columns are one-based; the end column is exclusive. These fixtures define the contract for the future compiler conformance runner and may be curated before that runner exists.

CTest validates the manifest structure and confirms that every expected primary span selects the documented source token.
