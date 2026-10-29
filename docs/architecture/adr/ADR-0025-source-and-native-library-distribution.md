# ADR-0025: Separate OMBasic source libraries from native binary libraries

-   Status: Accepted
-   Date: 2026-10-29

## Context

OMBasic has two distinct reuse boundaries: source modules compiled as
part of the consuming program, and native libraries exposing functions
through a platform C ABI. Source imports and native symbol resolution
must remain separate. The architecture already defines strict
source-tree import paths, `.omi` C ABI metadata, native linking, and a
boundary between generated-code ABI and Runtime implementation.

## Decision

### 1. OMBasic source libraries are distributed as source trees

-   OMBasic source reuse is source-level. Programs import `.bas` modules
    using `IMPORT`.
-   A reusable source library is an ordinary directory subtree
    containing `.bas` files and optional documentation, licenses, tests,
    or resources. It is not a first-class compiler entity.
-   Project source imports follow ADR-0020. An SDK may provide one fixed
    standard source tree under `share/ombasic/stdlib`, available only through
    the explicit `@std/` import root; this is not a search path.
-   Source libraries may be distributed through Git repositories, source
    archives, vendored files, or other external file-distribution
    mechanisms. The compiler does not download, install, update, or
    resolve them.
-   This ADR introduces no language-level `Package`, `package.toml`,
    package registry, dependency resolver, or dependency lockfile. Any
    future distribution tooling requires a separate decision.

### 2. Native binary libraries use the Native interface

-   Native libraries may be distributed as platform-specific static or
    shared artifacts, including `.a`, `.lib`, `.so`, `.dylib`, and
    `.dll`, subject to target toolchain support.
-   Native symbols are described by `.omi` compile-time metadata and
    resolved through the Native semantic-analysis and link-resolution
    pipeline in ADR-0012.
-   `.omi` describes the supported C ABI boundary. It is compile-time
    metadata, not a runtime module, package manifest, or shared-library
    loader.
-   The build uses explicit native-linking configuration and the
    selected target descriptor. Platform-specific import libraries,
    system libraries, and shared-library deployment requirements remain
    explicit build concerns.
-   Static and dynamic linking are both allowed where supported. This
    ADR does not promise every library format on every platform.

### 3. `IMPORT` never loads native binaries

-   `IMPORT` refers only to OMBasic source modules.
-   DLL, SO, dylib, static archive, or import-library paths are never
    used in an `IMPORT` statement.
-   Source-module resolution and native symbol resolution are separate
    compiler operations with separate diagnostics.

### 4. OMBasic 0.1 does not define a stable OMBasic binary ABI

-   OMBasic 0.1 defines no general binary module format for precompiled
    OMBasic libraries consumed by other OMBasic programs.
-   No `.omlib` artifact or equivalent precompiled OMBasic module format
    is introduced.
-   OMBasic-specific values and runtime-managed
    representations---including strings, collections, actor references,
    result values, and GC-managed objects---must not be assumed to have
    a stable external ABI.
-   The generated-code Runtime ABI in ADR-0023 is an internal
    compiler-to-Runtime boundary, not a promise that arbitrary OMBasic
    libraries can be distributed as independently compiled binary
    modules.
-   A future feature exporting OMBasic functionality through a
    restricted C ABI may be considered separately. It is not a general
    OMBasic-to-OMBasic binary ABI and requires its own design decision.

## Consequences

-   Source reuse remains inspectable and is compiled with the consuming
    program, allowing full source-level analysis.
-   Native reuse has an explicit C ABI boundary described by `.omi` and
    resolved during compilation and linking.
-   `IMPORT` has one meaning and does not conflate source modules with
    operating-system dynamic libraries.
-   The project need not freeze OMBasic object layouts, GC ownership
    rules, actor representations, symbol mangling, or Runtime internals
    to support native C libraries.
-   Applications distributing dynamic libraries must ship or install the
    required target-specific shared artifacts and satisfy platform
    runtime-loader rules. A successful link alone does not guarantee
    that a deployed executable can locate every shared library.
-   Source-library version selection and provenance remain external to
    the compiler, for example through Git commits, release archives, or
    vendored source snapshots.

## Rejected alternatives

### Use `IMPORT` for source modules and native libraries

Rejected because source-tree resolution and platform-native
library/symbol resolution are different mechanisms.

### Define a stable OMBasic binary module ABI in 0.1

Rejected because it would prematurely constrain runtime-managed values,
memory ownership, actor references, calling conventions, symbol naming,
and compiler/Runtime evolution.

### Require all native libraries to be static

Rejected because some system and third-party libraries are distributed
or intended to be consumed as shared libraries. Static linking may
simplify deployment but is not the only valid model.

### Introduce a package manager as part of this decision

Rejected as out of scope. Source-library distribution does not require
package identity, version solving, registries, or automatic fetching in
the compiler.

## Implementation requirements

-   Keep source import resolution consistent with ADR-0020.
-   Keep `.omi` as compile-time Native interface metadata, consistent
    with ADR-0012.
-   Keep native linking and artifact responsibilities consistent with
    ADR-0016.
-   Use target descriptor and sysroot rules from ADR-0024 when selecting
    and validating native artifacts.
-   Keep generated-code ABI declarations separate from Runtime
    implementation headers, consistent with ADR-0023.
-   Test source import failures separately from Native declaration,
    ABI-classification, link, and runtime shared-library deployment
    failures.
-   Diagnostics for unsupported or mismatched native artifacts should
    identify the target and the missing or incompatible artifact rather
    than silently searching unrelated directories.

## Related decisions

-   ADR-0012: Native C ABI calls described by OMI
-   ADR-0016: Native linking and artifact model
-   ADR-0020: Resolve imports strictly beneath the source root
-   ADR-0023: Separate generated Runtime ABI from Runtime implementation
-   ADR-0024: Make target and sysroot per-compilation inputs
