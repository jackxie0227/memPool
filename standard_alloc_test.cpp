#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <new>

struct alignas(64) CacheLineObject
{
    char data[64];
};

struct alignas(8192) PageAlignedObject
{
    char data[8192];
};

static bool IsAligned(void *ptr, size_t alignment)
{
    return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
}

int main()
{
    for (size_t size = 1; size <= 256; ++size)
    {
        void *ptr = malloc(size);
        assert(ptr != nullptr);
        assert(IsAligned(ptr, alignof(std::max_align_t)));
        free(ptr);
    }

    void *zero = malloc(0);
    assert(zero != nullptr);
    assert(IsAligned(zero, alignof(std::max_align_t)));
    free(zero);
    free(nullptr);

    char *cleared = static_cast<char *>(calloc(32, 4));
    assert(cleared != nullptr);
    for (size_t i = 0; i < 128; ++i)
    {
        assert(cleared[i] == 0);
    }

    char *grown = static_cast<char *>(realloc(cleared, 256));
    assert(grown != nullptr);
    memset(grown, 0x5a, 256);
    char *shrunk = static_cast<char *>(realloc(grown, 64));
    assert(shrunk != nullptr);
    free(shrunk);

    for (size_t alignment : {size_t(32), size_t(64), size_t(256), size_t(1) << 12, size_t(1) << 13, size_t(1) << 20})
    {
        void *posixPtr = nullptr;
        assert(posix_memalign(&posixPtr, alignment, 100) == 0);
        assert(posixPtr != nullptr);
        assert(IsAligned(posixPtr, alignment));
        free(posixPtr);

        void *alignedPtr = aligned_alloc(alignment, alignment);
        assert(alignedPtr != nullptr);
        assert(IsAligned(alignedPtr, alignment));
        free(alignedPtr);
    }

    char *one = new char;
    assert(IsAligned(one, __STDCPP_DEFAULT_NEW_ALIGNMENT__));
    delete one;

    long double *ld = new long double;
    assert(IsAligned(ld, alignof(long double)));
    delete ld;

    char *array = new char[97];
    assert(IsAligned(array, __STDCPP_DEFAULT_NEW_ALIGNMENT__));
    delete[] array;

    CacheLineObject *cacheLineObj = new CacheLineObject;
    assert(IsAligned(cacheLineObj, 64));
    delete cacheLineObj;

    PageAlignedObject *pageObj = new PageAlignedObject;
    assert(IsAligned(pageObj, 8192));
    delete pageObj;

    std::cout << "standard allocation replacement test passed\n";
    return 0;
}
