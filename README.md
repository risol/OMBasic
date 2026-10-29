# OMBasic 0.1

OMBasic is a statically typed, BASIC-style language with explicit effects and
Actor concurrency. Source is compiled ahead of time to a native executable:

```text
.bas -> semantic analysis -> typed HIR -> native MIR -> LLVM -> executable
```

The language rules are in the
[OMBasic Language Standard](OMBasic-Language-Standard.md). The installed SDK
contains the `ombasic` compiler, public Runtime headers, and target Runtime
archives. See the [SDK resource layout](docs/compiler/resource_layout.md) for
the installed files and external toolchain requirements.

## Install and build a program

Install the OMBasic SDK and put its `bin/` directory on `PATH`. Building a
native program also requires LLVM 23 Clang/LLD and the host C development
toolchain. On Windows, use `clang-cl` with Visual Studio C++ Build Tools and a
Windows SDK; MinGW is not part of the supported toolchain.

From the directory containing `main.bas`:

```sh
ombasic check main.bas
ombasic build main.bas -o app
./app
```

On Windows PowerShell, use `ombasic.exe`, build with `-o app.exe`, and run
the result as `./app.exe`.

The Hello World and Actor examples are in [`examples/hello`](examples/hello)
and [`examples/actor`](examples/actor). `ombasic run main.bas` compiles a
temporary native executable and runs it.

## Distributed Actors

OMBasic 0.1 distributed Actor transport does not provide peer authentication,
transport encryption, or an Internet-facing security boundary unless the
deployment environment supplies them. Distribution is intended for trusted
or otherwise secured networks. Compatibility fingerprints identify matching
program and Message contracts; they do not authenticate peers or authorize
operations. See [ADR-0014](docs/architecture/adr/ADR-0014-distribution-descriptors-fingerprints-and-transport.md)
for the transport contract.

## Development

Compiler build instructions, contribution rules, and release gates are in the
[contribution guide](docs/contributing.md) and
[release gates](docs/development/release_gates.md). The
[documentation index](docs/README.md) links the language, compiler, Runtime,
and architecture documents.
