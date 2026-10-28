# OMBasic SDK resource layout

The compiler resolves Runtime archives from an SDK resource root at link time.
It does not embed build-tree archive paths.

## Installed layout

```text
<sdk>/
├── bin/
│   └── ombasic[.exe]
├── include/ombasic/
├── lib/ombasic/<target-triple>/
│   ├── ombasic_runtime[.lib|.a]
│   ├── ombasic_runtime_services[.lib|.a]
│   ├── ombasic_runtime_distribution[.lib|.a]
│   ├── ombasic_distribution[.lib|.a]
│   └── ombasic_foundation[.lib|.a]
└── share/ombasic/
    ├── target-manifest.json
    └── tools/
        ├── ombasic_bind.py
        └── ombasic_lsp.py
```

The manifest stores relative resource paths and the target triple used to build
this SDK. Installed static archives have debug records stripped so they do not
retain paths from the machine that built the SDK.

## Resource root selection

For `build` and `run`, OMBasic resolves resources in this order:

1. `--resource-dir <sdk-root>`
2. `OMBASIC_RESOURCE_DIR`
3. The parent directory of the executable directory (installed `bin/` layout)
4. The executable directory itself (build-tree SDK layout)

An explicit option or environment override is authoritative. If its target
Runtime archives are missing, OMBasic reports the target, root and expected
archive path instead of searching another location. `check` performs semantic
analysis and does not require Runtime archives.

The target Runtime directory is:

```text
<sdk-root>/lib/ombasic/<target-triple>/
```

The current compiler selects the host triple configured at compiler build time.
Per-compilation `--target` and `--sysroot` are tracked separately in the target
model work.

## External tools

LLVM 23 Clang/LLD and the host C runtime toolchain remain external requirements.
The linker driver is found by its platform command name (`clang-cl` on Windows,
`clang` on Linux), so its `bin` directory must be on `PATH`. Python 3 must be on
`PATH` for `bind` and `lsp`. On Windows, `clang-cl` uses the installed Visual
Studio C++ toolchain and Windows SDK to resolve MSVC/UCRT libraries. The SDK
does not bundle LLVM, Visual Studio, Python, or their absolute install paths.
