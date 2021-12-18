# Heap implementation

This project is an implementation of a heap (free store). The functions implemented are:
* `void* my_malloc(size_t size, size_t align = 1)` (allocate `size` bytes with alignment `align`)
* `void my_free(void* addr)` (deallocate/free memory at `addr` that was previously allocated by `my_malloc`)
* `void my_heap_dump()` (print heap blocks to stdout)
* `void my_leak_check()` (try to find memory leaks, at least on the regular heap)
* various overloads of `new`/`delete` operators to check if this works with standard containers
    * They are removed from windows builds as they clash with `libstdc++`-provided definitions. A simple static array of pointers is used instead to stress an allocator a bit.

## Build

This project requires a C++17 compiler. It's tested on Linux (GCC and MinGW cross-compiling with Wine). It will *probably* work on MSVC.

Building on Linux:
```sh
mkdir build
cd build
cmake .. -GNinja
ninja -j4
```
Run with:
```sh
./heap 
```


## How this works?

The free store consists of page-aligned heap blocks, of size 4 pages (16 KiB, assuming 4 KiB pages). A heap block can be considered a doubly-linked list node - pointers to previous and next blocks + data. The first block is a global variable (static storage duration) and has previous pointer always NULL.

Ths heap blocks are further divided into variable-sized regions that are bounded with headers (signature + region size, 8 bytes total). A signature specified, what is the state of this block (see `heap.cpp:22`).

The block data area is initialized to the following state:
```
0               8             SIZE-8              SIZE
| Header(EMPTY) | 0xefefefef... | Header(END_EDGE) |
```
(`0xefefefef` are scrub bytes to ease finding of uninitialized heap accesses)

When an `N`-byte allocation is done, the following steps are taken:
1. Check if an allocation will fit in a block. If not, just request N + sizeof(header) bytes from the OS (`mmap()`)
2. Align the size to required align
3. Iterate on headers searching for some free space (it must hold 2 headers + `N` bytes of data)
4. When a place is found, create a new header after data and make previous header pointing to the new header.
5. If no place is found, try allocating in a next block
6. If there is no new block, request it from the OS and append to a block list.

Example: We want to allocate `N`=400 bytes in an empty heap. The heap state after allocation is:
```
                        N + 8           N + 16
0               8        408             416      SIZE-8            SIZE
| Header(USED)  | 0xef... | Header(EMPTY) | 0xef... | Header(END_EDGE) |
```

When we want to free an `address`, the following steps are taken:
1. Find a block that the address is allocated on.
2. Find the header of this address (it is `address - sizeof(header)`)
3. Remove the header AFTER this "base" header, and save that "base" header data was freed (to allow some basic double-free detection)
4. Merge together blocks that were freed.
5. If a block is empty (consists only of EMPTY/FREE and END_EDGE header) and was requested from the OS, remove it (with `munmap()`).

Example: Deallocate previously allocated 400 bytes. The heap state will be:
```
                        N + 8           N + 16
0                8        408             416      SIZE-8            SIZE
| Header(FREED)  | 0xef... | Header(EMPTY) | 0xef... | Header(END_EDGE) |
```
after merge:
```
0                8       SIZE-8            SIZE
| Header(FREED)  | 0xef... | Header(END_EDGE) |
```
