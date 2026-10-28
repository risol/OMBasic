# Safe C23 profile

The compiler and runtime use C23 but remain subject to explicit project rules:

- use <stdckdint.h> through checked size helpers before allocation arithmetic;
  MSVC builds use equivalent checked size operations in
  src/foundation/memory/checked_size.h because the MSVC C runtime does not
  currently ship <stdckdint.h>;
- only memory, arena, vector, and runtime heap implementations may call raw allocators;
- never use VLA, alloca, gets, strcpy, strcat, or sprintf;
- represent byte ranges with pointer-plus-length views;
- use typed IDs for persistent cross-phase references;
- initialize every public object fully and document ownership;
- use assertions only for internal invariants, never malformed source input;
- constrain pointer arithmetic to Foundation containers and explicit platform/C ABI boundaries.

The architecture checker enforces raw allocator placement, unsafe APIs, forbidden includes, and cross-capability private-header access. New rules belong here and in the checker.
