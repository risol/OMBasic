# Random

`@std/random/random.bas` currently exposes a deterministic Park–Miller PRNG.
Its algorithm is the minimal standard recurrence with multiplier `16807` and
modulus `2147483647`:

```text
next = (state * 16807) mod 2147483647
```

`seed(Int)` normalizes the signed seed into the generator's non-zero state
range; zero maps to state 1. `next_sample(Int)` returns
`Random_Sample(state, value)`, where
`value` is the same Int as the new state and lies in `[1, 2147483646]`. The
state is explicit, so identical seeds produce identical sequences across
platforms using checked Int64 arithmetic. The product is bounded below Int64
maximum.

This generator is reproducible but is **not cryptographically secure**. No
system entropy or hidden global state is used.

For secret bytes, send `Secure_Random_Bytes(count, reply)` to the
`Secure_Random` Actor. Its `Secure_Random_Bytes_Completed` carries
`Result<List<Int>, String>`; successful values contain bytes in 0..255. Wrap
the list with `encoding.buffer_from_bytes` when a Buffer is needed. Stable
error strings are `invalid_length`, `resource_exhausted`,
`entropy_unavailable`, and `unsupported`. The count limit is 64 MiB. This
Actor reads the OS CSPRNG and never falls back to the deterministic PRNG.
Runtime shutdown suppresses outstanding completions after cancellation.

See [ADR-0039](../architecture/adr/ADR-0039-random-model.md) for platform
sources and protocol details.
