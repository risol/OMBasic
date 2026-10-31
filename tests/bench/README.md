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

Compare a no-order fused Query filter/projection with a hand-written loop on
the same integer input:

```sh
python3 tests/bench/query_scan_benchmark.py \
  build/linux/clang/release/ombasic --rows 256 --iterations 200 --rounds 10
```

The benchmark builds both programs before timing and checks their checksums.
Each program filters into a `List<Int>` and folds that list into the same
checksum. The Query uses its internal result append path; the hand-written
loop uses list concatenation because the language has no public mutable-list
append operation. Process startup is included in each sample, with one untimed
warmup run.

On the local single-core Linux x64 Release container with LLVM 23.1.4,
`--rows 256 --iterations 200 --rounds 10` measured 4.139 ms for Query and
56.233 ms for the equivalent list-building hand loop (Query at 0.07x the loop
time). This difference primarily reflects the loop's repeated list
concatenation; the language currently exposes no mutable-list append operation.
Treat this as an implementation comparison, not a claim that Query beats a
hand loop with the same internal append primitive.
