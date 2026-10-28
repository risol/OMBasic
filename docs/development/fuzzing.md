# Fuzzing

Fuzz targets use Clang's libFuzzer with AddressSanitizer and
UndefinedBehaviorSanitizer. They cover source import paths and lexing/parsing,
`.omi` metadata, distribution frames and wire payloads, constructed HIR and
MIR verifier inputs, and the Runtime's descriptor-to-native-message codec
round trip. A separate low-budget target emits LLVM objects from verified MIR
without executing the resulting code.

In the existing Docker validation container, the already-extracted Linux LLVM
is mounted at `/opt/llvm` from `F:\LLVM-23.1.4-Linux-X64`:

```sh
cmake -S /workspace -B /build/fuzz -G Ninja \
  -DCMAKE_C_COMPILER=/opt/llvm/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/llvm/bin/clang++ \
  -DLLVM_DIR=/opt/llvm/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DBUILD_TESTING=ON \
  -DOM_SANITIZER=address-undefined \
  -DOM_BUILD_FUZZERS=ON \
  -DOM_LLVM_TARGET_TRIPLE=x86_64-unknown-linux-gnu
cmake --build /build/fuzz --parallel
```

Each target accepts an optional corpus directory and a run budget. A short
smoke run is:

```sh
for fuzzer in \
  ombasic_fuzz_syntax \
  ombasic_fuzz_metadata \
  ombasic_fuzz_import_path \
  ombasic_fuzz_distribution \
  ombasic_fuzz_hir_verifier \
  ombasic_fuzz_mir_verifier \
  ombasic_fuzz_distribution_serializer
do
  /build/fuzz/"$fuzzer" -runs=10000
done
/build/fuzz/ombasic_fuzz_llvm_backend -runs=100
/build/fuzz/ombasic_fuzz_native_descriptor_codec -runs=10000
```

The HIR harness builds a bounded project with leaf nodes, then mutates node,
child, and type references. The MIR harness uses the public builder API to
construct a scalar function and mutates bounded verifier fields. The backend
harness builds and verifies bounded scalar MIR, emits an LLVM object, and
removes the temporary object. The descriptor-codec harness builds bounded
Message descriptors and native values, encodes them, decodes them, and checks
that scalar and String fields round-trip. No fuzzer executes generated native
code.
