#include "ConcurrentAlloc.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <new>

namespace
{
    bool IsPowerOfTwo(size_t value)
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    bool MulOverflow(size_t a, size_t b, size_t &out)
    {
        if (a != 0 && b > std::numeric_limits<size_t>::max() / a)
        {
            return true;
        }
        out = a * b;
        return false;
    }

    size_t PageSize()
    {
        return static_cast<size_t>(1) << PAGE_SHIFT;
    }

    size_t PagesForBytes(size_t bytes)
    {
        return SizeClass::_RoundUp(bytes, PageSize()) >> PAGE_SHIFT;
    }

    size_t UsableSizeForRequest(size_t size)
    {
        if (size == 0)
        {
            size = 1;
        }
        if (size > MAX_BYTES)
        {
            return SizeClass::RoundUp(size);
        }
        return SizeClass::RoundUp(size);
    }

    size_t UsableSizeForPointer(void *ptr)
    {
        Span *span = PageCache::GetInstance()->MapObjectToSpan(ptr);
        // Pointer-validity checks are handled in later refactor stages.
        return SpanUsableSize(span);
    }

    void *AllocateStandard(size_t size) noexcept
    {
        try
        {
            // 最少申请 1 个字节
            return tcmalloc(size == 0 ? 1 : size);
        }
        catch (...) // 异常时设置errno 返回空指针
        {
            errno = ENOMEM;
            return nullptr;
        }
    }

    void *AllocatePageAligned(size_t alignment, size_t size) noexcept
    {
        if (size == 0)
        {
            size = 1;
        }

        size_t k = PagesForBytes(size); // size 对应的页数(至少一页)
        // 此前已经确保 alignment>PAGESIZE(4KB)
        size_t alignPages = alignment >> PAGE_SHIFT; // 对齐页数

        try
        {
            std::lock_guard<std::mutex> lock(PageCache::GetInstance()->GetMtx());
            Span *span = PageCache::GetInstance()->NewAligned(k, alignPages);
            if (span != nullptr)
            {
                // 页级对齐分配没有切成小对象。_objSize 记录可用容量，
                // realloc 可以据此决定是否原地复用
                assert(span->_state == SpanState::Direct);
                span->use_count = 0;
                span->_freeList = nullptr;
                span->_objSize = span->_n << PAGE_SHIFT;
            }
            // 返回span起始内存地址
            return span == nullptr ? nullptr : (void *)(span->_pageID << PAGE_SHIFT);
        }
        catch (...)
        {
            errno = ENOMEM;
            return nullptr;
        }
    }

    void *AllocateAligned(size_t alignment, size_t size) noexcept
    {
        // 对齐值至少大于DEFAULT_ALIGN
        // 即ThreadCache和CentralCache中桶的最小大小
        if (alignment < DEFAULT_ALIGN)
        {
            alignment = DEFAULT_ALIGN;
        }

        // 对齐值需要是2的幂
        if (!IsPowerOfTwo(alignment))
        {
            errno = EINVAL;
            return nullptr;
        }

        // 对齐值小于页大小
        // 不在块内部移动用户指针，只把 size 向上修正到 alignment 倍数
        // 因为块起点来自页对齐 span，块大小也是 alignment 倍数，所以每个块起点都满足 alignment
        // 如 size=100 alignment=64 -> ptrsize=128
        if (alignment <= PageSize())
        {
            size_t roundedSize = SizeClass::_RoundUp(size == 0 ? 1 : size, alignment);
            return AllocateStandard(roundedSize);
        }

        // 大于页大小的对齐必须由 PageCache 返回对齐后的 Span 起点。
        // 这样 free/delete 仍可直接通过页号找到 Span 并释放，不需要 header。
        return AllocatePageAligned(alignment, size);
    }

    void *Reallocate(void *ptr, size_t size) noexcept
    {
        if (ptr == nullptr) // 原来为空指针 重新申请一块内存
        {
            return AllocateStandard(size);
        }
        if (size == 0) // 调整大小为0，释放原内存并返回空指针
        {
            tcfree(ptr);
            return nullptr;
        }

        // 比较可用容量
        // 如果前后都在同一个大小的桶中则直接返回原地址
        // 否则申请新的大小为size的内存并释放原内存
        size_t oldUsable = UsableSizeForPointer(ptr);
        size_t newUsable = UsableSizeForRequest(size);
        if (newUsable == oldUsable)
        {
            return ptr;
        }

        void *newPtr = AllocateStandard(size);
        if (newPtr == nullptr)
        {
            return nullptr;
        }

        memcpy(newPtr, ptr, oldUsable < size ? oldUsable : size);
        tcfree(ptr);
        return newPtr;
    }
}

extern "C" void *malloc(size_t size) noexcept // todo except的作用
{
    return AllocateStandard(size);
}

// todo 以下情况是否会导致崩溃
// 1. free栈内存
// 2. new 与 free 混用
// 3. malloc 与 delete 混用
// 4. double free
extern "C" void free(void *ptr) noexcept
{
    tcfree(ptr);
}

// 申请 n 个元素，每个元素占 size 字节的内存，初始化为零
extern "C" void *calloc(size_t n, size_t size) noexcept
{
    size_t bytes = 0;                // 一共需申请的字节数
    if (MulOverflow(n, size, bytes)) // size_t 溢出检查
    {
        errno = ENOMEM;
        return nullptr;
    }

    void *ptr = AllocateStandard(bytes);
    if (ptr != nullptr)
    {
        memset(ptr, 0, bytes); // 初始化为零
    }
    return ptr;
}

// 调整一块已有内存的大小
extern "C" void *realloc(void *ptr, size_t size) noexcept
{
    return Reallocate(ptr, size);
}

// 申请一块满足指定对其要求的内存
// 大小为 size  按照 alignment 对齐的内存
extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size) noexcept
{
    // 对齐要求至少大于 sizeof(void*) 且是 2 的幂
    // alignment 8 16 32 64 ...
    // 对齐要求非法时返回 EINVAL
    if (alignment < sizeof(void *) || !IsPowerOfTwo(alignment))
    {
        return EINVAL;
    }

    void *ptr = AllocateAligned(alignment, size);
    if (ptr == nullptr)
    {
        return ENOMEM;
    }
    *memptr = ptr;
    return 0;
}

extern "C" void *aligned_alloc(size_t alignment, size_t size) noexcept
{
    if (alignment < sizeof(void *) || !IsPowerOfTwo(alignment) || size % alignment != 0)
    {
        errno = EINVAL;
        return nullptr;
    }
    return AllocateAligned(alignment, size);
}

void *operator new(std::size_t size)
{
    void *ptr = AllocateStandard(size);
    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new[](std::size_t size)
{
    void *ptr = AllocateStandard(size);
    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return AllocateStandard(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return AllocateStandard(size);
}

void operator delete(void *ptr) noexcept
{
    tcfree(ptr);
}

void operator delete[](void *ptr) noexcept
{
    tcfree(ptr);
}

void operator delete(void *ptr, std::size_t) noexcept
{
    tcfree(ptr);
}

void operator delete[](void *ptr, std::size_t) noexcept
{
    tcfree(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
    tcfree(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept
{
    tcfree(ptr);
}

#if __cplusplus >= 201703L
void *operator new(std::size_t size, std::align_val_t align)
{
    void *ptr = AllocateAligned(static_cast<size_t>(align), size);
    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new[](std::size_t size, std::align_val_t align)
{
    void *ptr = AllocateAligned(static_cast<size_t>(align), size);
    if (ptr == nullptr)
    {
        throw std::bad_alloc();
    }
    return ptr;
}

void *operator new(std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept
{
    return AllocateAligned(static_cast<size_t>(align), size);
}

void *operator new[](std::size_t size, std::align_val_t align, const std::nothrow_t &) noexcept
{
    return AllocateAligned(static_cast<size_t>(align), size);
}

void operator delete(void *ptr, std::align_val_t) noexcept
{
    tcfree(ptr);
}

void operator delete[](void *ptr, std::align_val_t) noexcept
{
    tcfree(ptr);
}

void operator delete(void *ptr, std::size_t, std::align_val_t) noexcept
{
    tcfree(ptr);
}

void operator delete[](void *ptr, std::size_t, std::align_val_t) noexcept
{
    tcfree(ptr);
}

void operator delete(void *ptr, std::align_val_t, const std::nothrow_t &) noexcept
{
    tcfree(ptr);
}

void operator delete[](void *ptr, std::align_val_t, const std::nothrow_t &) noexcept
{
    tcfree(ptr);
}
#endif
