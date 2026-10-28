# ADR-0017: Stable diagnostics and native debug information

Status: Accepted

## Context

Native AOT diagnostics must connect compiler and Runtime failures back to
source code. Debug information must survive LLVM object emission and the
platform linker without becoming a Runtime dependency.

## Decision

- Give diagnostics stable stage-based codes and one-based source positions;
  columns count UTF-8 bytes, and spans have inclusive starts and exclusive
  ends. Preserve code and primary-span behavior as wording evolves.
- Keep diagnostic code ranges grouped by compiler stage and never reuse a code
  for a different error. Compile-fail conformance fixtures assert codes and
  primary spans.
- Retain source spans on lowered MIR instructions and construct a static
  source-span table for Runtime fault reporting.
- Emit LLVM source, function, and line metadata from OMBasic source locations.
- Use CodeView metadata and a sibling PDB on Windows, and DWARF on Linux.
- Keep native debugger metadata in the object/executable and do not require the
  compiler or LLVM to be present when running the program.

## Consequences

- Native debuggers can map machine frames and Runtime faults to OMBasic source
  files and lines.
- Diagnostic codes remain suitable for conformance tests and editor clients.
- Debug information is a build artifact; it does not change Runtime ABI.
