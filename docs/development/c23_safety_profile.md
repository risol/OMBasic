# Safe C23 profile

The compiler and runtime use C23 but remain subject to explicit project rules:

- use <stdckdint.h> through checked size helpers before allocation arithmetic;
- only memory, arena, vector, and runtime heap implementations may call raw allocators;
- never use VLA, alloca, gets, strcpy, strcat, or sprintf;
- represent byte ranges with pointer-plus-length views;
- use typed IDs for persistent cross-phase references;
- initialize every public object fully and document ownership;
- use assertions only for internal invariants, never malformed source input;
- constrain pointer arithmetic to Foundation containers, bytecode decoding, and OS/FFI boundaries.

The architecture checker enforces raw allocator placement, unsafe APIs, forbidden includes, and cross-capability private-header access. New rules belong here and in the checker.
