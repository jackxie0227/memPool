#include "common.h"
#include "ObjectPool.h"
#include "PageMap.h"

#define private public
#include "PageCache.h"
#undef private

#include "ConcurrentAlloc.h"

#include <cassert>
#include <cstdlib>
#include <iostream>
#include <mutex>

namespace
{
    Span *RawPageMapLookup(PageID page)
    {
        return static_cast<Span *>(PageCache::GetInstance()->_idSpanMap.get(page));
    }
}

int main()
{
    {
        std::lock_guard<std::mutex> lock(PageCache::GetInstance()->GetMtx());
        Span *direct = PageCache::GetInstance()->NewSpan(3, SpanState::Direct);
        assert(direct != nullptr);
        assert(direct->_state == SpanState::Direct);
        assert(direct->_n == 3);

        PageID first = direct->_pageID;
        PageID middle = direct->_pageID + 1;
        PageID last = direct->_pageID + direct->_n - 1;

        assert(RawPageMapLookup(first) == direct);
        assert(RawPageMapLookup(last) == direct);
        assert(RawPageMapLookup(middle) != direct);

        PageCache::GetInstance()->ReleaseSpanToPageCache(direct);
    }

    void *small = malloc(200000);
    assert(small != nullptr);

    Span *smallSpan = PageCache::GetInstance()->MapObjectToSpan(small);
    assert(smallSpan != nullptr);
    assert(smallSpan->_state == SpanState::Small);
    assert(smallSpan->_n >= 3);

    PageID first = smallSpan->_pageID;
    PageID middle = smallSpan->_pageID + smallSpan->_n / 2;
    PageID last = smallSpan->_pageID + smallSpan->_n - 1;
    void *middlePageAddress = reinterpret_cast<void *>(middle << PAGE_SHIFT);

    {
        std::lock_guard<std::mutex> lock(PageCache::GetInstance()->GetMtx());
        assert(RawPageMapLookup(first) == smallSpan);
        assert(RawPageMapLookup(middle) == smallSpan);
        assert(RawPageMapLookup(last) == smallSpan);
    }
    assert(PageCache::GetInstance()->MapObjectToSpan(middlePageAddress) == smallSpan);

    free(small);

    std::cout << "span pagemap range test passed\n";
    return 0;
}
