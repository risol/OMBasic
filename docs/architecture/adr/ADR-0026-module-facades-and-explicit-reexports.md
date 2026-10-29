# ADR-0026: Module Facades and Explicit Re-exports

- Status: Accepted
- Date: 2026-10-29

## Context

Source libraries are ordinary source trees (ADR-0025). Their consumers need
one stable public entry point without introducing packages, directory imports,
or implicit discovery. OMBasic modules are individual `.bas` files and Import
paths are rooted at the project Source Root.

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
- Paths without a `./` prefix remain Source Root-relative. A `./` prefix is
  resolved from the importing module's directory, then canonicalized and
  validated inside the same Source Root. No directory search is introduced.
- No `@std` root, package manager, manifest, or lockfile is introduced.
- Standard-library source, when distributed by an SDK, must be copied or
  vendored beneath the consuming project's Source Root before ordinary import.
- The `print` builtin and native library boundary are unchanged.

## Consequences

The parser and loader must agree on the import-prefix grammar. Name resolution
must retain original symbol identity and declaration locations while exposing a
module's deterministic public surface. Interface fingerprints must describe
that surface rather than private implementation files.
