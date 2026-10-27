# OMBasic reference implementation

OMBasic is a statically typed, BASIC-style language with explicit effects and Actor concurrency. The language rules are in OMBasic-Language-Standard.md; the implementation plan is OMBasic-Compiler-Development-Plan.md.

Architecture, ABI, protocol, testing, and contribution references are indexed
in [docs/README.md](docs/README.md).

## Build

The project uses C23, CMake, and CTest. Configure with a C23 compiler and a generator such as Ninja:

~~~powershell
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target architecture-check
~~~

Sanitizer profiles are configured with `OM_SANITIZER=address-undefined`,
`thread`, or `memory`. Run the sanitizer profiles in a supported Linux
Clang/GCC environment. Some Docker kernels require non-PIE TSan builds and
disabled address randomization for TSan processes.

### Native FFI dependency

The Native FFI uses libffi for calls whose C signatures come from `.omi`
metadata. Install the libffi development headers and library before configuring
CMake. Set `OM_LIBFFI_ROOT` to the installation prefix when automatic discovery
does not find them. On Windows with MSYS2 MinGW64, install the development
package with `pacman -S mingw-w64-x86_64-libffi`; keep its `bin` directory on
`PATH` so programs linked to its DLL can run.
