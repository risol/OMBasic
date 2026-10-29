# ADR-0028: Module-scoped Native imports

- Status: Accepted
- Date: 2026-10-29

## Context

The `.omi` ABI already supports borrowed OMBasic Strings through `CString`,
managed String results, ownership annotations, effects, and blocking metadata.
The compiler currently injects every command-line `.omi` declaration into the
global Prelude. That prevents a standard-library module from using Native
String primitives without making those functions visible to every source
module.

## Decision

- A source module may declare `NATIVE IMPORT "path.omi"` in its import preamble.
- The path uses the same explicit Project-root-relative, current-module-relative
  `./`, or SDK-root-relative `@std/` selection as a source Import, and must end
  in `.omi`. It is canonicalized and checked inside the selected root.
- A Native Import's functions and Native Records are available only in the
  importing `.bas` module's module scope. They are not added to the Prelude.
- Native declarations cannot be exported or re-exported. A `.bas` module may
  expose ordinary OMBasic wrappers with `EXPORT` and `EXPORT IMPORT`.
- A Native Import loads metadata for semantic checking; it does not execute
  code. `check` does not require the Native library to be linkable. `build` and
  `run` link the imported library using the existing target ABI and effect
  rules.
- A Native metadata file is not an OMBasic source module, does not enter the
  source Module Graph, and does not change the `.bas` Module identity.
- Existing command-line `--native` remains an explicit project-wide host
  integration mechanism. SDK standard-library implementations must use
  module-scoped Native Imports rather than `--native` or Prelude injection.

## Consequences

The parser records Native Import declarations separately from source Imports.
The check pipeline resolves each metadata path using the importing module's
root kind, loads and validates `.omi`, and supplies its declarations to the
resolver only for that module. Native dependencies must remain alive through
type inference, lowering, and linking. Module interfaces expose only explicitly
exported OMBasic wrappers and retain stable logical source paths.

Positive and negative conformance tests must cover module visibility, private
Native names, invalid paths, absent metadata, duplicate declarations, check
without linking, and Native AOT linking.
