# ADR-0026: Module Facades and Explicit Re-exports

- Status: Accepted
- Date: 2026-10-29

## Context

Source libraries are ordinary source trees (ADR-0025). Their consumers need
one stable public entry point without introducing packages, directory imports,
or implicit discovery. OMBasic modules are individual `.bas` files. Project and
SDK standard-library roots are separate identity and security boundaries.

## Decision

- A directory is an organizational convention, never a language-level Module.
- A directory `x/` may use `x/x.bas` as its documented Facade. Consumers still
  explicitly import that file; the compiler does not scan directories.
- `EXPORT IMPORT "path.bas"` imports the target and re-exports its public
  declarations by their existing names. It does not export the target module
  alias or synthesize declarations.
- Re-exports compose transitively. Private declarations are never re-exported.
- Re-export imports participate in the ordinary module dependency graph; cycles
  remain compile-time errors.
- Re-exporting the same original symbol more than once is harmless. Distinct
  symbols with the same exported name in one module are a compile-time conflict.
- Project-relative paths resolve below the Project Source Root. `./` resolves
  below the importing Module directory while preserving its root kind.
- `@std/` explicitly selects the chosen SDK's `share/ombasic/stdlib` root.
  Standard-library modules may use `./` or another explicit `@std/` path;
  they may not import from the consuming project's source root.
- Root kind plus canonical relative path forms Module identity. No directory
  search, fallback, package manager, dependency solver, or lockfile is added.
- The `print` builtin and native library boundary are unchanged.

## Consequences

The parser and loader must agree on the import-prefix grammar. Name resolution
must retain original symbol identity and declaration locations while exposing a
module's deterministic public surface. Interface fingerprints must describe
that surface rather than private implementation files.
