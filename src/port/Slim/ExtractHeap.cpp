#include "ExtractHeap.h"

#ifdef __3DS__
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <malloc.h>
#include <new>

#include <spdlog/spdlog.h>

extern "C" {
#include <3ds/types.h>
#include <3ds/svc.h>
#include <3ds/synchronization.h>
}

// Blocks of up to kMaxSmall bytes come from 64 KB chunks, each holding one block size, with a
// free list in each chunk. The thread that called ExtractHeap_Begin uses them without a lock;
// any other thread's free goes on a list that thread takes back later. A chunk goes back to
// malloc once it's empty, so memory one size no longer needs can be used for another.
namespace {
constexpr uintptr_t kChunkSize = 64 * 1024;
// The normal heap is mapped from 0x08000000, and is at most 128 MB
constexpr uintptr_t kHeapBase = 0x08000000;
constexpr uintptr_t kHeapEnd = 0x10000000;
constexpr size_t kMaxChunks = (kHeapEnd - kHeapBase) / kChunkSize;
constexpr size_t kMaxSmall = 256;
constexpr int kClasses = 16;
constexpr uint16_t kClassSize[kClasses] = { 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256 };

struct FreeBlock {
    FreeBlock* next;
};

struct Chunk {
    Chunk* prev; // in its class's list of chunks with free blocks
    Chunk* next;
    FreeBlock* free;
    char* bump; // blocks past this have never been handed out
    char* end;
    uint32_t live;
    uint16_t blockSize;
    uint8_t cls;
    bool listed;
};
constexpr size_t kHeaderSize = (sizeof(Chunk) + 7) & ~size_t(7);

struct SizeClass {
    Chunk* current;
    Chunk* partial; // other chunks with free blocks
};

// Read by every operator delete, so a pointer from malloc is told apart with one load
uint8_t sSlabChunk[kMaxChunks];
SizeClass sClasses[kClasses];
volatile bool sActive = false;
void* sOwner = nullptr; // the owning thread's TLS pointer, which is unique per thread
// Blocks freed by other threads while active, pushed without a lock
FreeBlock* volatile sRemote = nullptr;
// Frees after ExtractHeap_End, from any thread
LightLock sAfterLock;
bool sAfterLockReady = false;

size_t sChunks = 0;
size_t sPeakChunks = 0;
size_t sAllocs = 0;

inline int ClassOf(size_t n) {
    if (n <= 64) {
        return (int)((n + 7) / 8) - 1;
    }
    if (n <= 128) {
        return 8 + (int)((n - 65) / 16);
    }
    return 12 + (int)((n - 129) / 32);
}

inline size_t ChunkIndex(const void* p) {
    return ((uintptr_t)p - kHeapBase) / kChunkSize;
}

inline bool IsSlab(const void* p) {
    const size_t index = ChunkIndex(p);
    return index < kMaxChunks && sSlabChunk[index] != 0;
}

inline Chunk* ChunkOf(const void* p) {
    return (Chunk*)((uintptr_t)p & ~(kChunkSize - 1));
}

void Unlist(SizeClass& sc, Chunk* c) {
    if (c->prev != nullptr) {
        c->prev->next = c->next;
    } else {
        sc.partial = c->next;
    }
    if (c->next != nullptr) {
        c->next->prev = c->prev;
    }
    c->prev = c->next = nullptr;
    c->listed = false;
}

void List(SizeClass& sc, Chunk* c) {
    c->prev = nullptr;
    c->next = sc.partial;
    if (sc.partial != nullptr) {
        sc.partial->prev = c;
    }
    sc.partial = c;
    c->listed = true;
}

void ReleaseChunk(Chunk* c) {
    sSlabChunk[ChunkIndex(c)] = 0;
    sChunks--;
    free(c);
}

Chunk* NewChunk(int cls) {
    void* memory = memalign(kChunkSize, kChunkSize);
    if (memory == nullptr) {
        return nullptr;
    }
    if ((uintptr_t)memory < kHeapBase || (uintptr_t)memory + kChunkSize > kHeapEnd) {
        free(memory);
        return nullptr;
    }
    Chunk* c = (Chunk*)memory;
    c->prev = c->next = nullptr;
    c->free = nullptr;
    c->bump = (char*)memory + kHeaderSize;
    c->end = (char*)memory + kChunkSize;
    c->live = 0;
    c->blockSize = kClassSize[cls];
    c->cls = (uint8_t)cls;
    c->listed = false;
    sSlabChunk[ChunkIndex(c)] = 1;
    if (++sChunks > sPeakChunks) {
        sPeakChunks = sChunks;
    }
    return c;
}

// A free on the owning thread while active, or under sAfterLock afterwards
void FreeBlockLocal(void* p, bool active) {
    Chunk* c = ChunkOf(p);
    FreeBlock* block = (FreeBlock*)p;
    block->next = c->free;
    c->free = block;
    c->live--;
    if (!active) {
        if (c->live == 0) {
            ReleaseChunk(c);
        }
        return;
    }
    SizeClass& sc = sClasses[c->cls];
    if (c == sc.current) {
        return;
    }
    if (c->live == 0) {
        if (c->listed) {
            Unlist(sc, c);
        }
        ReleaseChunk(c);
    } else if (!c->listed) {
        List(sc, c);
    }
}

void TakeRemoteFrees() {
    FreeBlock* block = __atomic_exchange_n(&sRemote, (FreeBlock*)nullptr, __ATOMIC_ACQUIRE);
    while (block != nullptr) {
        FreeBlock* next = block->next;
        FreeBlockLocal(block, true);
        block = next;
    }
}

void* SlabAlloc(size_t n) {
    SizeClass& sc = sClasses[ClassOf(n)];
    Chunk* c = sc.current;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (c != nullptr) {
            if (FreeBlock* block = c->free; block != nullptr) {
                c->free = block->next;
                c->live++;
                return block;
            }
            if (c->bump + c->blockSize <= c->end) {
                void* block = c->bump;
                c->bump += c->blockSize;
                c->live++;
                return block;
            }
        }
        // The current chunk is full: use one with free blocks, or a new one
        if (sRemote != nullptr) {
            TakeRemoteFrees();
        }
        if (sc.partial != nullptr) {
            c = sc.partial;
            Unlist(sc, c);
        } else {
            c = NewChunk(ClassOf(n));
            if (c == nullptr) {
                return nullptr;
            }
        }
        sc.current = c;
    }
    return nullptr;
}

inline bool OnOwner() {
    return sActive && getThreadLocalStorage() == sOwner;
}

void* MallocOrThrow(size_t n) {
    for (;;) {
        if (void* p = malloc(n != 0 ? n : 1)) {
            return p;
        }
        std::new_handler handler = std::get_new_handler();
        if (handler == nullptr) {
            throw std::bad_alloc();
        }
        handler();
    }
}

inline void* New(size_t n) {
    if (n - 1 < kMaxSmall && OnOwner()) {
        sAllocs++;
        if (void* p = SlabAlloc(n)) {
            return p;
        }
    }
    return MallocOrThrow(n);
}

inline void Delete(void* p) {
    if (p == nullptr) {
        return;
    }
    if (!IsSlab(p)) {
        free(p);
        return;
    }
    if (OnOwner()) {
        FreeBlockLocal(p, true);
    } else if (sActive) {
        FreeBlock* block = (FreeBlock*)p;
        FreeBlock* head = __atomic_load_n(&sRemote, __ATOMIC_RELAXED);
        do {
            block->next = head;
        } while (!__atomic_compare_exchange_n(&sRemote, &head, block, true, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    } else {
        LightLock_Lock(&sAfterLock);
        FreeBlockLocal(p, false);
        LightLock_Unlock(&sAfterLock);
    }
}
} // namespace

void ExtractHeap_Begin() {
    if (!sAfterLockReady) {
        LightLock_Init(&sAfterLock);
        sAfterLockReady = true;
    }
    sOwner = getThreadLocalStorage();
    sAllocs = 0;
    sPeakChunks = sChunks;
    __atomic_store_n(&sActive, true, __ATOMIC_RELEASE);
}

void ExtractHeap_End() {
    if (!sActive) {
        return;
    }
    // Other threads' frees from now on wait for sAfterLock. Ones pushed before that are taken
    // back while the class lists still hold every chunk. A thread that saw sActive just before
    // it was cleared can still push after this; that block is never reused, which only leaks it.
    LightLock_Lock(&sAfterLock);
    __atomic_store_n(&sActive, false, __ATOMIC_RELEASE);
    TakeRemoteFrees();
    // Chunks still holding blocks (statics made while active) stay until those are freed
    for (SizeClass& sc : sClasses) {
        while (sc.partial != nullptr) {
            Chunk* c = sc.partial;
            Unlist(sc, c);
            if (c->live == 0) {
                ReleaseChunk(c);
            }
        }
        Chunk* current = sc.current;
        sc.current = nullptr;
        if (current != nullptr && current->live == 0) {
            ReleaseChunk(current);
        }
    }
    const size_t left = sChunks;
    LightLock_Unlock(&sAfterLock);
    SPDLOG_INFO("Extract heap: {} small allocations, at most {} KB in chunks, {} KB still in use", sAllocs,
                sPeakChunks * kChunkSize / 1024, left * kChunkSize / 1024);
}

void* operator new(size_t n) {
    return New(n);
}

void* operator new[](size_t n) {
    return New(n);
}

void* operator new(size_t n, const std::nothrow_t&) noexcept {
    try {
        return New(n);
    } catch (...) {
        return nullptr;
    }
}

void* operator new[](size_t n, const std::nothrow_t&) noexcept {
    try {
        return New(n);
    } catch (...) {
        return nullptr;
    }
}

void operator delete(void* p) noexcept {
    Delete(p);
}

void operator delete[](void* p) noexcept {
    Delete(p);
}

void operator delete(void* p, size_t) noexcept {
    Delete(p);
}

void operator delete[](void* p, size_t) noexcept {
    Delete(p);
}

void operator delete(void* p, const std::nothrow_t&) noexcept {
    Delete(p);
}

void operator delete[](void* p, const std::nothrow_t&) noexcept {
    Delete(p);
}

#else

void ExtractHeap_Begin() {
}

void ExtractHeap_End() {
}

#endif
