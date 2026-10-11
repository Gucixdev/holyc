# alloc

Two allocators that are worth having besides `malloc`, on a replaceable allocator hook.

- **arena**: bump allocation (a pointer add, 16-byte aligned), freed all at once. `mark`/`rewind`
  gives a stack discipline for temporaries; `reset` keeps the first chunk so a steady state
  does not go back to the OS. Pointers stay valid until `rewind`/`reset`.
- **slab**: O(1) alloc/free of fixed-size blocks from a free list, chunked growth, blocks never
  move, double free is detected.
- **mem**: the single hook (`mem.h`) both sit on, so a host application can supply its own
  allocator and aligned allocation works on POSIX and MinGW/MSVC.

Neither is thread-safe; give each thread its own. `make test` builds and runs `tests/test_arena.c`.
