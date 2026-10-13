// ============================================================================
// Two-Level Segregated Fit 分配器
// 块布局: [Block header][payload]
//   header.size 低位: 1 = free, 0 = used
//   header.size 高位: 整块字节数(含 header)
//   payload 起始 = header + sizeof(Block)，按 ALIGN(默认 16B) 对齐
// ============================================================================
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include "SpinLock.h"

class TLSFAllocator {
private:
    SpinLock m_lock;
public:
    static constexpr int    SL_LOG2 = 5;
    static constexpr int    ALIGN_LOG2 = 4;
    static constexpr int    FL_MAX = 40;
    static constexpr size_t ALIGN = size_t(1) << ALIGN_LOG2;

    void* alloc(size_t size) {
        SpinLockGuard g(m_lock);
        return alloc_locked(size);
    }
    void dealloc(void* ptr) {
        SpinLockGuard g(m_lock);
        dealloc_locked(ptr);
    }
    void* realloc(void* ptr, size_t new_size) {
        SpinLockGuard g(m_lock);
        return realloc_locked(ptr, new_size);
    }
    void init(void* mem, size_t bytes) {
        SpinLockGuard g(m_lock);
        init_locked(mem, bytes);
    }

    size_t total_size() const { return m_size; }
    bool   valid()      const { return m_head != nullptr; }



private:
    static constexpr int    SL_COUNT = 1 << SL_LOG2;
    static constexpr int    FL_SHIFT = SL_LOG2 + ALIGN_LOG2;
    static constexpr int    FL_COUNT = FL_MAX - FL_SHIFT + 1;
    static constexpr size_t SMALL_BLOCK = size_t(1) << FL_SHIFT;

    struct Block {
        Block* prev_phys;
        Block* next_free;
        Block* prev_free;
        size_t size;
    };

    Block* m_head{ nullptr };
    Block* m_tail{ nullptr };
    size_t   m_size{ 0 };
    uint32_t m_fl_bitmap{ 0 };
    uint32_t m_sl_bitmap[FL_COUNT]{};
    Block* m_buckets[FL_COUNT][SL_COUNT]{};

    static int fls(size_t x) {
#if defined(_MSC_VER)
        unsigned long idx = 0; _BitScanReverse64(&idx, x); return (int)idx;
#else
        return 63 - __builtin_clzll(x);
#endif
    }
    static int ffs(uint32_t x) {
#if defined(_MSC_VER)
        unsigned long idx = 0; _BitScanForward(&idx, x); return (int)idx;
#else
        return __builtin_ctz(x);
#endif
    }

    static size_t block_size(const Block* b) { return b->size & ~size_t(1); }
    static bool   block_is_free(const Block* b) { return (b->size & 1) != 0; }
    static void   block_set_free(Block* b) { b->size |= 1; }
    static void   block_set_used(Block* b) { b->size &= ~size_t(1); }
    static Block* block_next(const Block* b) {
        return (Block*)((uintptr_t)b + block_size(b));
    }
    static size_t align_up(size_t n) {
        return (n + ALIGN - 1) & ~(ALIGN - 1);
    }

    void   mapping_insert(size_t size, int* fl, int* sl) const;
    void   mapping_search(size_t size, int* fl, int* sl) const;
    Block* search_suitable(int* fl, int* sl);
    void   insert_free(Block* b);
    void   remove_free(Block* b, int fl, int sl);

 
    void* alloc_locked(size_t size);
    void   dealloc_locked(void* ptr);
    void* realloc_locked(void* ptr, size_t new_size);
    void   init_locked(void* mem, size_t bytes);
};

// ============================= 实现 =============================

inline void TLSFAllocator::mapping_insert(size_t size, int* fl, int* sl) const {
    if (size < SMALL_BLOCK) {
        *fl = 0;
        *sl = (int)(size / (SMALL_BLOCK / SL_COUNT));
    }
    else {
        int k = fls(size);
        *sl = (int)((size >> (k - SL_LOG2)) ^ (size_t(1) << SL_LOG2));
        int f = k - (FL_SHIFT - 1);
        if (f < 0)          f = 0;
        if (f >= FL_COUNT)  f = FL_COUNT - 1;
        *fl = f;
    }
}

inline void TLSFAllocator::mapping_search(size_t size, int* fl, int* sl) const {
    if (size >= SMALL_BLOCK) {
        size_t round = (size_t(1) << (fls(size) - SL_LOG2)) - 1;
        size += round;
    }
    mapping_insert(size, fl, sl);
}

inline void TLSFAllocator::insert_free(Block* b) {
    int fl, sl;
    mapping_insert(block_size(b), &fl, &sl);

    b->prev_free = nullptr;
    b->next_free = m_buckets[fl][sl];
    if (b->next_free) b->next_free->prev_free = b;
    m_buckets[fl][sl] = b;

    m_fl_bitmap |= (1u << fl);
    m_sl_bitmap[fl] |= (1u << sl);
    block_set_free(b);
}

inline void TLSFAllocator::remove_free(Block* b, int fl, int sl) {
    Block* prev = b->prev_free;
    Block* next = b->next_free;

    if (prev) prev->next_free = next;
    else      m_buckets[fl][sl] = next;
    if (next) next->prev_free = prev;

    if (!m_buckets[fl][sl]) {
        m_sl_bitmap[fl] &= ~(1u << sl);
        if (!m_sl_bitmap[fl]) m_fl_bitmap &= ~(1u << fl);
    }
    b->prev_free = b->next_free = nullptr;
}

inline TLSFAllocator::Block* TLSFAllocator::search_suitable(int* fl, int* sl) {
    int f = *fl;
    int s = *sl;

    uint32_t sl_map = m_sl_bitmap[f] & (~0u << s);
    if (!sl_map) {
        uint32_t fl_map = m_fl_bitmap & (~0u << (f + 1));
        if (!fl_map) return nullptr;
        f = ffs(fl_map);
        *fl = f;
        sl_map = m_sl_bitmap[f];
    }
    s = ffs(sl_map);
    *sl = s;
    return m_buckets[f][s];
}

inline void TLSFAllocator::init_locked(void* memory, size_t bytes) {
    m_head = m_tail = nullptr;
    m_size = 0;
    m_fl_bitmap = 0;
    for (int i = 0; i < FL_COUNT; ++i) {
        m_sl_bitmap[i] = 0;
        for (int j = 0; j < SL_COUNT; ++j) m_buckets[i][j] = nullptr;
    }
    if (!memory || bytes < 4 * sizeof(Block)) return;

    uintptr_t start = (uintptr_t)memory;
    uintptr_t end = start + bytes;
    start = (start + ALIGN - 1) & ~(uintptr_t)(ALIGN - 1);
    end = end & ~(uintptr_t)(ALIGN - 1);
    if (end < start + 4 * sizeof(Block)) return;

    m_head = (Block*)start;
    m_head->prev_phys = nullptr;
    m_head->next_free = nullptr;
    m_head->prev_free = nullptr;
    m_head->size = sizeof(Block);

    Block* main_block = (Block*)(start + sizeof(Block));
    m_tail = (Block*)(end - sizeof(Block));
    size_t main_size = (uintptr_t)m_tail - (uintptr_t)main_block;
    main_block->prev_phys = m_head;
    main_block->size = main_size | 1;

    m_tail->prev_phys = main_block;
    m_tail->next_free = nullptr;
    m_tail->prev_free = nullptr;
    m_tail->size = sizeof(Block);

    m_size = end - start;
    insert_free(main_block);
}

inline void* TLSFAllocator::alloc_locked(size_t size) {
    if (!m_head || size == 0) return nullptr;

    size_t payload = align_up(size);
    size_t needed = sizeof(Block) + payload;
    if (fls(needed) >= FL_MAX) return nullptr;

    int fl, sl;
    mapping_search(needed, &fl, &sl);
    Block* b = search_suitable(&fl, &sl);
    if (!b) return nullptr;

    remove_free(b, fl, sl);

    size_t bsize = block_size(b);
    if (bsize >= needed + sizeof(Block)) {
        Block* rest = (Block*)((uintptr_t)b + needed);
        size_t rest_size = bsize - needed;
        rest->prev_phys = b;
        rest->size = rest_size | 1;

        Block* after = (Block*)((uintptr_t)rest + rest_size);
        after->prev_phys = rest;

        b->size = needed;
        insert_free(rest);
    }
    else {
        b->size = bsize;
    }

    return (void*)((uintptr_t)b + sizeof(Block));
}

inline void TLSFAllocator::dealloc_locked(void* ptr) {
    if (!ptr) return;
    Block* b = (Block*)((uintptr_t)ptr - sizeof(Block));

    block_set_free(b);

    Block* next = block_next(b);
    if (block_is_free(next)) {
        int fl, sl;
        mapping_insert(block_size(next), &fl, &sl);
        remove_free(next, fl, sl);

        b->size = (block_size(b) + block_size(next)) | 1;

        Block* after = block_next(b);
        after->prev_phys = b;
    }

    Block* prev = b->prev_phys;
    if (block_is_free(prev)) {
        int fl, sl;
        mapping_insert(block_size(prev), &fl, &sl);
        remove_free(prev, fl, sl);

        prev->size = (block_size(prev) + block_size(b)) | 1;

        Block* after = block_next(prev);
        after->prev_phys = prev;
        b = prev;
    }

    insert_free(b);
}

inline void* TLSFAllocator::realloc_locked(void* ptr, size_t new_size) {
    if (!ptr)          return alloc_locked(new_size);
    if (new_size == 0) { dealloc_locked(ptr); return nullptr; }

    Block* b = (Block*)((uintptr_t)ptr - sizeof(Block));
    size_t old_payload = block_size(b) - sizeof(Block);
    size_t new_payload = align_up(new_size);

    if (new_payload <= old_payload) return ptr;

    size_t needed = sizeof(Block) + new_payload;

    Block* next = block_next(b);
    if (block_is_free(next) &&
        block_size(b) + block_size(next) >= needed)
    {
        int fl, sl;
        mapping_insert(block_size(next), &fl, &sl);
        remove_free(next, fl, sl);

        size_t combined = block_size(b) + block_size(next);

        if (combined >= needed + sizeof(Block)) {
            Block* rest = (Block*)((uintptr_t)b + needed);
            size_t rest_size = combined - needed;
            rest->prev_phys = b;
            rest->size = rest_size | 1;

            Block* after = (Block*)((uintptr_t)rest + rest_size);
            after->prev_phys = rest;

            b->size = needed;
            insert_free(rest);
        }
        else {
            b->size = combined;
            Block* after = block_next(b);
            after->prev_phys = b;
        }
        return ptr;
    }

    void* np = alloc_locked(new_size);
    if (!np) return nullptr;
    std::memcpy(np, ptr, old_payload);
    dealloc_locked(ptr);
    return np;
}