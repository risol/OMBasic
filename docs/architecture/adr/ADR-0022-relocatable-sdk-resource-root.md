# ADR-0022: Resolve SDK resources from a relocatable resource root

- Status: Accepted
- Date: 2026-10-29

## Context

The compiler currently receives absolute build-tree Runtime archive paths as
compile definitions. An installed compiler therefore depends on the original
development build directory.

## Decision

Runtime archives and manifests are installed beneath a relocatable OMBasic SDK
root. Resource lookup precedence is CLI override, `OMBASIC_RESOURCE_DIR`, then
the executable-relative installed layout. If no candidate contains the
requested target resources, compilation fails with a diagnostic naming the
target, candidates, missing resource, and expected path. The working directory
and arbitrary PATH scans do not influence resource selection. `--sysroot` is
separate from the OMBasic resource root.

## Consequences

- Link requests consume resolved resources instead of compile-time archive
  macros.
- An install-and-move test is required to establish relocatability.
- `check` remains usable without installed Runtime archives.
