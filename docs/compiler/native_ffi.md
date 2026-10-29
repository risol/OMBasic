# Native FFI

Native declarations are supplied with `.omi` metadata. `check` loads them for
name resolution and type checking; `build` and `run` also link the library
declared by each metadata file.

Use a module-level `NATIVE IMPORT "path.omi"` when the declarations should be
visible only to one `.bas` module. Those declarations are private to that
module; export ordinary OMBasic wrappers when a library wants to expose a
stable source API. The command-line `--native` option remains available for
explicit project-wide integrations.

```text
ombasic run --root app app/main.bas --native native/math.omi -L build/native
```

Library names are translated to platform link inputs: `native_math` becomes
`-lnative_math` on Linux and `native_math.lib` on Windows. A library path with
a path separator or an archive/shared-library suffix is passed directly to
the linker. Each `-L dir` adds a search directory.

## Scalar ABI

The backend directly declares C symbols in LLVM and lowers these `.omi` types:

- `CInt8` through `CInt64`, `CUInt8` through `CUInt64`, `CInt`, `CUInt`,
  `CLong`, `CULong`, `CLongLong`, `CULongLong`, and `CSize`;
- `CBool`, `CFloat`, `CDouble`, and `CVoid` returns.

`CLong` and `CULong` follow the selected target ABI: 32 bits on Windows and
pointer width on Linux. `CSize` follows target pointer width. Narrow integers
are truncated on call and extended according to signedness on return; LLVM
extension attributes are emitted for narrow C parameters and results.

## CString

CString parameters must be marked `BORROWED`. The Runtime rejects embedded NUL
bytes and copies the UTF-8 contents to a NUL-terminated stack buffer that stays
valid through the native call. CString returns require a `RETURNS` ownership
annotation and are copied into a managed OMBasic String before the native
pointer can expire.

Borrowed and static results are copied directly. Owned results name a release
function; the Runtime invokes it after the copy attempt, including when UTF-8
validation or managed-string allocation fails. A null return or invalid UTF-8
raises a recoverable runtime fault.

Only Native functions declared both `EFFECT PURE` and `NONBLOCKING` may be
called from ordinary OMBasic `FUNCTION`s. Other Native effects remain subject
to the language's normal effect restrictions. `PURE` is a library-author
contract and is not verified against the external implementation.

```text
FUNCTION echo(value: CString BORROWED): CString
    SYMBOL "native_echo"
    EFFECT PURE
    NONBLOCKING
    RETURNS BORROWED FROM value
END FUNCTION

FUNCTION duplicate(value: CString BORROWED): CString
    SYMBOL "native_duplicate"
    EFFECT PURE
    NONBLOCKING
    RETURNS OWNED WITH release_string
END FUNCTION

FUNCTION release_string(value: CPointer OWNED): CVoid
    SYMBOL "native_release"
    EFFECT PURE
    NONBLOCKING
END FUNCTION
```

CPointer is a managed opaque resource handle. Its raw address exists only
inside a generated Native call wrapper. BORROWED and STATIC parameters borrow
the address for the call; OWNED parameters transfer the owned address to C and
disarm the managed handle. An owned result is wrapped with its declared release
function and released when the handle becomes unreachable or is explicitly
transferred to an OWNED Native parameter. Borrowed results must name a non-owned
CPointer parameter; the result handle keeps that owner alive. Static results
have no release callback or managed owner.

The backend does not expose raw addresses to BASIC expressions, lower
by-value Native Records as direct source-level fields, or support callback
parameters yet. It also does not use libffi.

## Native Record by-value calls

An `.omi` `NATIVE RECORD` can be used as a native function parameter or result.
OMBasic carries it as an opaque, nominal value and passes it back through
Native calls; source code cannot construct one or read its fields yet. The LLVM
backend computes field size, alignment, and offsets from the selected target
data layout, then classifies the aggregate for that target's C ABI.

The target-specific classifiers are SysV AMD64, Win64 x64, AAPCS64, and Darwin
ARM64. SysV AMD64 handles one- and two-eightbyte register aggregates,
including integer, floating-point, and pointer fields; aggregates larger than
16 bytes and memory-class layouts use indirect arguments or `sret` returns.
Win64 passes 1-, 2-, 4-, and 8-byte aggregates in an integer register and uses
indirect arguments or `sret` for other sizes. AAPCS64 and Darwin ARM64 pass
non-HFA aggregates up to 8 bytes in an integer register and aggregates from 9
through 16 bytes as two integer slots; larger non-HFA values use indirect
arguments or `sret`. Homogeneous floating-point aggregates of up to four
`float` or `double` fields use floating-point arrays, including when their size
exceeds 16 bytes. Linux AAPCS64 marks these array parameters with
`alignstack(8)`; Darwin ARM64 does not.

The AAPCS64 and Darwin ARM64 classifiers are covered by native ABI tests for
small integer records, partial 9-to-16-byte aggregates, mixed fields, indirect
records, and homogeneous float/double aggregates. Their LLVM signatures were
checked against Clang 23.1.4 output for Linux AArch64 and Darwin ARM64. The C
and C++ end-to-end roundtrip fixtures currently run on Linux x64 and Windows
x64; they cover two doubles, mixed integer/float fields, two floats, a 24-byte
aggregate, a pointer field, and a C++ `extern "C"` aggregate return and
parameter. Pointer fields copy the pointer value only; their lifetime and
ownership follow the `.omi` binding contract. Native Records remain opaque to
BASIC source, and nested Native Records are not supported.
