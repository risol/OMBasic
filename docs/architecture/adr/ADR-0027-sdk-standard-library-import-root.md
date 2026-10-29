# ADR-0027: Explicit SDK standard-library import root

- Status: Accepted
- Date: 2026-10-29

## Context

ADR-0025 distributes source libraries as ordinary `.bas` source. The SDK already
ships `share/ombasic/stdlib`, but requiring projects to copy it makes SDK
relocation and a stable standard-library version unnecessarily difficult.
Import paths still need deterministic root selection and a hard security
boundary.

## Decision

- `@std/` is the sole special import root and maps to the selected SDK's
  `share/ombasic/stdlib` directory. It is valid only in an Import path literal.
- Project-relative, current-module-relative, and `@std/` paths are explicit;
  there is no directory search or fallback.
- Module identity includes root kind and canonical relative path. Diagnostics
  use stable logical paths such as `@std/math/math.bas`, never SDK absolute
  paths.
- Standard-library `./` imports retain the SDK root. Standard-library modules
  cannot import project-root files.
- The existing exact-case, traversal, symlink/reparse-point, `.bas` suffix, and
  lower-snake-case checks apply independently to both roots.
- SDK release identity determines standard-library source identity. No package
  manager, version resolver, project lockfile, or binary OMBasic module format
  is introduced.

## Consequences

The loader and module graph must retain root identity through imports,
re-exports, diagnostics, caches, and SDK relocation. `check`, `build`, and `run`
must use the same SDK standard-library root. The standard library remains
ordinary source participating in the normal compiler pipeline.
