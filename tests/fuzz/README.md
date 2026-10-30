# Native compiler and protocol fuzzers

The optional libFuzzer targets exercise source import paths, the lexer/parser,
`.omi` metadata, distribution frame/message decoders, constructed HIR/MIR
verifier inputs, LLVM object emission from verified MIR, nested wire payloads,
and the Runtime's descriptor-to-native-message codec round trip, and JSON
parser validation/accessor/compaction behavior. Inputs are
bounded, and collection/nesting limits keep malformed inputs from requesting
unbounded work.

Foundation native boundaries also have dedicated targets for Number parsing,
UTF-8 hex decoding, and URI percent parsing/decoding.
The compression decoder target exercises bounded gzip and zlib-wrapped
DEFLATE parsing, including gzip optional-header handling and both checksum
paths.

Build with Clang and ASan/UBSan:

```sh
cmake -S . -B build/fuzz -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DLLVM_DIR=/path/to/llvm/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DOM_SANITIZER=address-undefined \
  -DOM_BUILD_FUZZERS=ON
cmake --build build/fuzz --parallel
```

Run a fuzzer for a bounded local smoke pass, or give it a saved corpus and a
longer time budget:

```sh
./build/fuzz/ombasic_fuzz_syntax -runs=10000
./build/fuzz/ombasic_fuzz_metadata -runs=10000
./build/fuzz/ombasic_fuzz_import_path -runs=10000
./build/fuzz/ombasic_fuzz_distribution -runs=10000
./build/fuzz/ombasic_fuzz_hir_verifier -runs=10000
./build/fuzz/ombasic_fuzz_mir_verifier -runs=10000
./build/fuzz/ombasic_fuzz_distribution_serializer -runs=10000
./build/fuzz/ombasic_fuzz_llvm_backend -runs=100
./build/fuzz/ombasic_fuzz_native_descriptor_codec -runs=10000
./build/fuzz/ombasic_fuzz_json_parser -runs=10000
./build/fuzz/ombasic_fuzz_number_parser -runs=10000
./build/fuzz/ombasic_fuzz_utf8_decoder -runs=10000
./build/fuzz/ombasic_fuzz_uri_parser -runs=10000
./build/fuzz/ombasic_fuzz_compression_decoder tests/fuzz/corpus/compression_decoder -runs=10000 -max_len=4096
./build/fuzz/ombasic_fuzz_regex_parser tests/fuzz/corpus/regex_parser -runs=10000 -max_len=2048
```

GitHub Actions builds every fuzzer and runs a short smoke set for each pull
request. A daily scheduled or manually dispatched workflow also runs a longer
per-fuzzer budget.
