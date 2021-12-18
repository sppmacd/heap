#pragma once

#include <stddef.h>

constexpr size_t heap_block_size = 1 << 14; // 16 KiB / 4 page

void* my_malloc(size_t size, size_t align = 1);
void my_free(void* addr);
void my_heap_dump();
void my_leak_check();

// Setup custom operators to see if this works for real code :)

// Placement new
void* operator new(size_t, void*) noexcept;
void* operator new[](size_t, void*) noexcept;

// New
void* operator new(size_t size);
void* operator new(size_t size, size_t align);
void* operator new[](size_t size);
void* operator new[](size_t size, size_t align);

// Delete
void operator delete(void* v) noexcept;
void operator delete(void* v, size_t) noexcept;
void operator delete[](void* v) noexcept;
void operator delete[](void* v, size_t) noexcept;
