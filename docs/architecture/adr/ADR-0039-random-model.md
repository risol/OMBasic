# ADR-0039: Random Model

- Status: Implemented
- Date: 2026-10-30

## Context

The existing `@std/random/random.bas` module provides a deterministic,
explicit-state Park–Miller PRNG. Applications that need secrets require system
entropy and must not mistake this reproducible generator for a secure source.

## Decision

1. Keep `seed` and `next_sample` pure. Their state remains an explicit `Int`
   value passed into and returned from each call. Identical seeds produce the
   same sequence on Linux and Windows.
2. Do not add implicit global PRNG state, clock-based default seeds, or a
   function that claims cryptographic security.
3. The `Secure_Random` system Actor accepts
   `Secure_Random_Bytes(count, reply)` and completes with
   `Result<List<Int>, String>`. The caller can wrap successful bytes with the
   Encoding Buffer constructor. The Runtime reads the operating system CSPRNG,
   enforces a 64 MiB cap, and has no fallback to the deterministic generator.
4. Unsupported platforms return `unsupported`; OS entropy failures return
   `entropy_unavailable`; invalid or oversized lengths return
   `invalid_length` or `resource_exhausted`. Service shutdown cancels queued
   requests and suppresses their accepted completions.
5. Secure output is not reproducible and is not suitable for conformance tests
   that assert exact bytes. Conformance asserts length, range, error behavior,
   and the cap; the platform CSPRNG selection is reviewed in the OS-specific
   Runtime implementation.

## Consequences

The pure PRNG and secure entropy Actor are implemented. Conformance covers
reproducibility, output range, zero-length output, invalid lengths, and the
resource cap on Linux and Windows.
