#include "heap.hpp"

#include <cstdint>      // uint32_t / u32
#include <assert.h>     // assert()
#ifdef WINDOWS
#   include "mman.h"       // mmap(), munmap()
#else
#   include <sys/mman.h>   // mmap(), munmap()
#endif
#include <stdlib.h>     // abort()
#include <stdio.h>      // printf(), perror()
#include <string.h>     // memset()

using u32 = uint32_t;

template<class T>
T max(T a, T b) { return a > b ? a : b; }

template<class T, size_t S>
T* end(T (&a)[S])
{
    return &a[S];
}

enum class Signature : u32
{
    Used =       0x2137D05A, // The memory is allocated.
    Empty =      0xE5425696, // The memory was never allocated and is available to use.
    EndEdge =    0xE57F402D, // Signature of the last header in the heap block.
    Freed =      0x2137DEAD, // The memory was freed.
    BigBlock =   0xB16C8056, // Signature of a big block (should not occur in HeapBlock)
    ScrubBytes = 0xDEDEDEDE, // There was previously a header, but it was removed (e.g. because of merge)
};

struct HeapHeader
{
    Signature signature;
    u32 size {};

    HeapHeader* next() const
    {
        return size == 0 ? nullptr : (HeapHeader*)((char*)this + size) + 1;
    }

    bool valid_signature() const
    {
        return signature == Signature::BigBlock
            || signature == Signature::Empty
            || signature == Signature::Used
            || signature == Signature::EndEdge
            || signature == Signature::Freed;
    }

    bool available() const
    {
        return signature == Signature::Empty
            || signature == Signature::Freed;
    }

    bool freed() const
    {
        return signature == Signature::Freed;
    }

    char const* signature_string() const
    {
        switch(signature)
        {
            case Signature::Used:       return "USED";
            case Signature::Empty:      return "EMPTY";
            case Signature::EndEdge:    return "END_EDGE";
            case Signature::Freed:      return "FREED";
            case Signature::BigBlock:   return "BIG_BLOCK";
            case Signature::ScrubBytes: return "SCRUB_BYTES";
            default:                    return nullptr;
        }
    }

    void print_signature() const
    {
        if(signature_string())
            printf("%s", signature_string());
        else
            printf("?0x%x?", signature);
    }
};

class HeapBlock
{
public:
    HeapBlock(HeapBlock* prev)
    : m_prev(prev)
    {
        init();
    }

    void* alloc(size_t size, size_t align);
    void free(void* addr);
    void leak_check();
    void dump();

private:
    void init();
    void place_edge_headers();
    void ensure_next_allocated_from_os();
    void merge_and_cleanup();

    HeapBlock* m_prev {};
    HeapBlock* m_next {};
    char m_data[heap_block_size - sizeof(void*) * 2];
};

static_assert(sizeof(HeapBlock) == heap_block_size);

void HeapBlock::place_edge_headers()
{
    new (m_data) HeapHeader {Signature::Empty, sizeof(m_data) - sizeof(HeapHeader) * 2};
    new (end(m_data) - sizeof(HeapHeader)) HeapHeader {Signature::EndEdge};
}

void HeapBlock::init()
{
    place_edge_headers();

    // Initialize rest of heap with scrub bytes
    memset(m_data + sizeof(HeapHeader), 0xef, sizeof(m_data) - sizeof(HeapHeader) * 2);
}

void HeapBlock::ensure_next_allocated_from_os()
{
    if(!m_next)
    {
        // Request a new memory block from the OS
        auto memory = (char*)mmap(nullptr, sizeof(HeapBlock), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        if(!memory)
        {
            perror("HeapBlock::alloc: mmap");
            abort();
        }

        m_next = reinterpret_cast<HeapBlock*>(memory);
        new(m_next) HeapBlock{this};
    }
}

void HeapBlock::merge_and_cleanup()
{
    // Merge adjacent blocks
    auto header = reinterpret_cast<HeapHeader*>(m_data);
    while(header)
    {
        if(header->available())
        {
            auto base_header = header;
            size_t size = header->size;
            while(header)
            {
                auto next_header = header->next();
                if(next_header && next_header->available())
                    size += next_header->size + sizeof(HeapHeader);
                else
                {
                    header = next_header;
                    break;
                }
                header = next_header;
            }
            base_header->size = size;
        }
        else
            header = header->next();
    }

    // Remove the whole block if it is made up from just freed blocks
    header = reinterpret_cast<HeapHeader*>(m_data);
    if(header->available() && header->size == sizeof(m_data) - sizeof(HeapHeader) * 2)
    {
        if(m_prev)
        {
            if(m_next)
                m_next->m_prev = m_prev;
            m_prev->m_next = m_next;
            // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
            // WARNING: This is very unsafe when done improperly.
            // Don't do ANYTHING after this instruction 
            // !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!
            if(munmap(this, sizeof(HeapBlock)) < 0)
            {
                perror("HeapBlock::free: munmap");
                abort();
            }
        }
    }
}

void* HeapBlock::alloc(size_t size, size_t align)
{
    if(align == 0)
    {
        printf("heap_alloc_impl: Invalid align!\n");
        return nullptr;
    }

    // Require at least 8-byte alignment
    align = max(sizeof(size_t), align);

    // Align size
    size_t old_size = size;
    //printf("Size before align %zx: %zu\n", align - 1, size);
    size &= ~(align - 1);
    if(size != old_size)
        size += align;
    // sanity check
    assert(old_size <= size);
    //printf("Size after align: %zu\n", size);

    HeapHeader* header = reinterpret_cast<HeapHeader*>(&m_data);

    while(header)
    {
        //printf("HEADER %x size=%u @%p next @%p\n", (uint32_t)header->signature, header->size, header, header->next());
        if(!header->valid_signature())
        {
            printf("heap_alloc_impl: Invalid header signature %x at %p\n", (u32)header->signature, header);
            abort();
        }
        if(header->available())
        {
            // Calculate total free size
            size_t free_size = header->size;
            //printf("free_size=%zu, header_size=%zu, required_size=%zu\n", free_size, sizeof(HeapHeader), size);
            if(free_size < size + sizeof(HeapHeader) * 2)
            {
                // skip and try next block
                //printf("Skipping\n");
            }
            else
            {
                //printf("Allocating %zu bytes\n", size);

                auto distance_to_next_header = header->size - size - sizeof(HeapHeader);

                // set old header to be used
                header->signature = Signature::Used;
                header->size = size;
                
                // create a new header after data
                auto new_header_address = header->next();
                new (new_header_address) HeapHeader {Signature::Empty, static_cast<u32>(distance_to_next_header)};
                return (void*)(header + 1);
            }
        }

        // Check for overflow
        if(header->signature == Signature::EndEdge)
        {
            ensure_next_allocated_from_os();
            return m_next->alloc(size, align);
        }

        // Try out next header, if it exists.
        header = header->next();
    }
    printf("HeapBlock::alloc: No suitable block found!\n");
    return nullptr;
}

void HeapBlock::free(void* addr)
{
    if(addr < m_data || addr >= end(m_data))
    {
        if(m_next)
            m_next->free(addr);
        else
        {
            printf("HeapBlock::free: %p was not allocated on heap\n", addr);
            abort();
        }
        return;
    }

    HeapHeader* header = reinterpret_cast<HeapHeader*>(addr) - 1;
    if(header->freed())
    {
        printf("HeapBlock::free: Block already freed\n");
        abort();
    }
    if(!header->valid_signature())
    {
        printf("HeapBlock::free: Invalid header signature %x (addr=%p)\n", (u32)header->signature, header);
        abort();
    }

    header->signature = Signature::Freed;
    merge_and_cleanup();
}

void HeapBlock::leak_check()
{
    printf("HeapBlock: Starting leak check on heap %p\n", this);

    // TODO: Check also big blocks??
    HeapHeader* header = reinterpret_cast<HeapHeader*>(m_data);
    bool leak_found = false;
    while(header)
    {
        if(header->signature == Signature::Used)
        {
            if(header->size > 0)
            {
                printf("(Leak check) Leaked %u bytes at %p\n", header->size, header + 1);
                leak_found = true;
            }
            header = header->next();
            continue;
        }
        if(!header->valid_signature())
        {
            printf("(Leak check) Heap corrupted at %p\n", header);
            leak_found = true;
            break;
        }
        header = header->next();
    }

    if(m_next)
        m_next->leak_check();
    if(!leak_found)
        printf("(Leak check) No leak found on heap %p. Congratulations!\n", this);
}

void HeapBlock::dump()
{
    HeapHeader* header = reinterpret_cast<HeapHeader*>(m_data);

    printf(" :: Heap %p; next = %p\n", this, m_next);
    while(header)
    {
        if(!header->valid_signature())
        {
            printf("(corrupted at %p, signature is %x)\n", header, (u32)header->signature);
            abort();
        }

        printf("    * %zu +%u",
            (reinterpret_cast<size_t>(header) - reinterpret_cast<size_t>(m_data)),
            header->size);
        if(header->next())
            printf(" next: %zu (%p)", reinterpret_cast<size_t>(header->next()) - reinterpret_cast<size_t>(m_data), header->next());

        if(header->available())
            printf(" (available)");
        if(header->freed())
            printf(" (freed)");
        printf(" ");
        header->print_signature();

        printf(" :: addr: %p\n", (header + 1));
        header = header->next();
    }
    if(m_next)
        m_next->dump();
}

// This is a hack to enable lazy-construction of heap also allowing to
// call member functions of HeapBlock
char g_heap_data[sizeof(HeapBlock)];
HeapBlock& g_heap_storage = *reinterpret_cast<HeapBlock*>(&g_heap_data);
bool g_heap_initialized { false };

void* my_malloc(size_t size, size_t align)
{
    if(!g_heap_initialized)
    {
        new (&g_heap_storage) HeapBlock{nullptr};
        g_heap_initialized = true;
    }
    assert(g_heap_initialized);

    if(size > sizeof(HeapBlock) - sizeof(HeapHeader))
    {
        //printf("my_malloc: Too big size: %zu, allocating big block!\n", size);
        auto memory = (char*)mmap(nullptr, size + sizeof(HeapHeader), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, 0, 0);
        if(!memory)
        {
            perror("my_malloc: mmap");
            return nullptr;
        }
        auto header = reinterpret_cast<HeapHeader*>(memory);
        header->signature = Signature::BigBlock;
        header->size = size + sizeof(HeapHeader);
        return header + 1;
    }
    return g_heap_storage.alloc(size, align);
}

void my_heap_dump()
{
    printf("----- HEAP DUMP BEGIN -----\n");
    if(!g_heap_initialized)
    {
        printf("(heap is not initialized)\n");
        return;
    }
    g_heap_storage.dump();
    printf("----- HEAP DUMP END -----\n");
}

void my_leak_check()
{
    g_heap_storage.leak_check();
}

void my_free(void* addr)
{
    HeapHeader* header = reinterpret_cast<HeapHeader*>(addr) - 1;
    if(header->signature == Signature::BigBlock)
    {
        printf("my_free: Freeing big block\n");
        munmap(header, header->size);
        return;
    }

    if(!g_heap_initialized)
    {
        printf("my_free: Heap is not initialized\n");
        abort();
    }

    //my_heap_dump();
    g_heap_storage.free(addr);
}

// Setup custom operators to see if this works for real code :)
// NOTE: Real examples are removed from windows builds as it
// would need disabling linking of standard library.
void* operator new(size_t, void* addr) noexcept
{
    return addr;
}
void* operator new[](size_t, void* addr) noexcept
{
    return addr;
}
void* operator new(size_t size)
{
    auto addr = my_malloc(size);
    //my_heap_dump();
    return addr;
}
void* operator new(size_t size, size_t align)
{
    auto addr = my_malloc(size, align);
    //my_heap_dump();
    return addr;
}
void* operator new[](size_t size)
{
    auto addr = my_malloc(size);
    //my_heap_dump();
    return addr;
}
void* operator new[](size_t size, size_t align)
{
    auto addr = my_malloc(size, align);
    //my_heap_dump();
    return addr;
}
void operator delete(void* v) noexcept
{
    my_free(v);
    //my_heap_dump();
}
void operator delete(void* v, size_t) noexcept
{
    my_free(v);
    //my_heap_dump();
}
void operator delete[](void* v) noexcept
{
    my_free(v);
    //my_heap_dump();
}
void operator delete[](void* v, size_t) noexcept
{
    my_free(v);
    //my_heap_dump();
}
