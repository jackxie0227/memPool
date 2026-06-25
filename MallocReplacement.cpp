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
        if (span->use_count == 0)
        {
            return span->_n << PAGE_SHIFT;
        }
        return span->_objSize;
    }

    void *AllocateStandard(size_t size) noexcept
    {
        try
        {
            return tcmalloc(size == 0 ? 1 : size);
        }
        catch (...)
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

        size_t k = PagesForBytes(size);
        size_t alignPages = alignment >> PAGE_SHIFT;

        try
        {
            std::lock_guard<std::mutex> lock(PageCache::GetInstance()->GetMtx());
            Span *span = PageCache::GetInstance()->NewAligned(k, alignPages);
            if (span != nullptr)
            {
                // 页级对齐分配没有切成小对象。_objSize 记录可用容量，
                // realloc 可以据此决定是否原地复用。
                span->_objSize = span->_n << PAGE_SHIFT;
            }
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
        if (alignment < DEFAULT_ALIGN)
        {
            alignment = DEFAULT_ALIGN;
        }
        if (!IsPowerOfTwo(alignment))
        {
            errno = EINVAL;
            return nullptr;
        }

        if (alignment <= PageSize())
        {
            // gperftools release 路径的策略：小于等于页大小的对齐请求，
            // 不在块内部移动用户指针，只把 size 向上修正到 alignment 倍数。
            // 因为块起点来自页对齐 span，块大小也是 alignment 倍数，所以每个块起点都满足 alignment。
            size_t roundedSize = SizeClass::_RoundUp(size == 0 ? 1 : size, alignment);
            return AllocateStandard(roundedSize);
        }

        // 大于页大小的对齐必须由 PageCache 返回对齐后的 Span 起点。
        // 这样 free/delete 仍可直接通过页号找到 Span 并释放，不需要 header。
        return AllocatePageAligned(alignment, size);
    }

    void *Reallocate(void *ptr, size_t size) noexcept
    {
        if (ptr == nullptr)
        {
            return AllocateStandard(size);
        }
        if (size == 0)
        {
            tcfree(ptr);
            return nullptr;
        }

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

extern "C" void *malloc(size_t size) noexcept
{
    return AllocateStandard(size);
}

extern "C" void free(void *ptr) noexcept
{
    tcfree(ptr);
}

extern "C" void *calloc(size_t n, size_t size) noexcept
{
    size_t bytes = 0;
    if (MulOverflow(n, size, bytes))
    {
        errno = ENOMEM;
        return nullptr;
    }

    void *ptr = AllocateStandard(bytes);
    if (ptr != nullptr)
    {
        memset(ptr, 0, bytes);
    }
    return ptr;
}

extern "C" void *realloc(void *ptr, size_t size) noexcept
{
    return Reallocate(ptr, size);
}

extern "C" int posix_memalign(void **memptr, size_t alignment, size_t size) noexcept
{
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
