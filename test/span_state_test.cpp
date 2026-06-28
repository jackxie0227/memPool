#include "ConcurrentAlloc.h"

#include <cassert>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iostream>

int main()
{
    Span freeSpan;
    freeSpan.Reset(123, 2, SpanState::Free);
    assert(freeSpan._state == SpanState::Free);
    assert(!freeSpan.use_count);
    assert(freeSpan._objSize == 0);
    assert(freeSpan._freeList == nullptr);

    Span emptySmallSpan;
    emptySmallSpan.Reset(456, 1, SpanState::Small);
    emptySmallSpan._objSize = 64;
    emptySmallSpan.use_count = 0;
    assert(emptySmallSpan._state == SpanState::Small);
    assert(emptySmallSpan._state != SpanState::Direct);
    assert(SpanUsableSize(&emptySmallSpan) == 64);

    void *small = malloc(48);
    assert(small != nullptr);
    Span *smallSpan = PageCache::GetInstance()->MapObjectToSpan(small);
    assert(smallSpan->_state == SpanState::Small);
    assert(smallSpan->_objSize == SizeClass::RoundUp(48));
    free(small);

    void *large = malloc(MAX_BYTES + 4096);
    assert(large != nullptr);
    Span *largeSpan = PageCache::GetInstance()->MapObjectToSpan(large);
    assert(largeSpan->_state == SpanState::Direct);
    assert(largeSpan->use_count == size_t{});
    assert(largeSpan->_freeList == nullptr);
    free(large);

    char *smallRealloc = static_cast<char *>(malloc(33));
    assert(smallRealloc != nullptr);
    std::memset(smallRealloc, 0x31, 33);
    Span *smallReallocSpan = PageCache::GetInstance()->MapObjectToSpan(smallRealloc);
    assert(smallReallocSpan->_state == SpanState::Small);
    char *sameSmall = static_cast<char *>(realloc(smallRealloc, 40));
    assert(sameSmall == smallRealloc);
    free(sameSmall);

    char *directRealloc = static_cast<char *>(malloc(MAX_BYTES + 4096));
    assert(directRealloc != nullptr);
    std::memset(directRealloc, 0x32, MAX_BYTES + 4096);
    Span *directReallocSpan = PageCache::GetInstance()->MapObjectToSpan(directRealloc);
    assert(directReallocSpan->_state == SpanState::Direct);
    char *sameDirect = static_cast<char *>(realloc(directRealloc, MAX_BYTES + 4096));
    assert(sameDirect == directRealloc);
    free(sameDirect);

    std::cout << "span state test passed\n";
    return 0;
}
