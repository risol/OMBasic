# Query benchmarks

Build and run the stable Top-K versus stable full-sort benchmark on Linux:

```sh
cmake -S . -B build/linux/clang/release -G Ninja \
  -DCMAKE_C_COMPILER=/opt/llvm-23/bin/clang \
  -DLLVM_DIR=/opt/llvm-23/lib/cmake/llvm \
  -DCMAKE_BUILD_TYPE=Release -DOM_BUILD_BENCHMARKS=ON
cmake --build build/linux/clang/release --target ombasic_query_order_benchmark
build/linux/clang/release/ombasic_query_order_benchmark 200000 100 5
```

Arguments are row count, K, and repetitions. The benchmark verifies that the
Top-K result equals the stable full-sort prefix before reporting CPU time. Its
selection workspace is O(K) for Top-K and O(N) for full sort; input and key
lists remain O(N) in both cases.

On the local Linux x64 Release build with LLVM 23.1.4, `N=200000`, `K=100`,
and five repetitions measured 19.762 ms for Top-K and 573.375 ms for full sort
(29.01x). This is an informational benchmark result, not a CI wall-clock gate.
