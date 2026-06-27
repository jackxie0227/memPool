#include "ConcurrentAlloc.h"

#include <cstdlib>

thread_local ThreadCache *pTLSThreadCache = nullptr;

namespace
{
    ThreadCache *GetThreadCache()
    {
        if (pTLSThreadCache == nullptr)
        {
            static ObjectPool<ThreadCache> objPool;
            objPool.getMtx().lock();
            pTLSThreadCache = objPool.New();
            objPool.getMtx().unlock();
        }
        return pTLSThreadCache;
    }
}

void *tcmalloc(size_t size)
{
    if (size == 0)
    {
        size = 1;
    }

    if (size > MAX_BYTES)
    {                                                // 单次申请空间大于 256KB
        size_t alignSize = SizeClass::RoundUp(size); // 空间对齐
        size_t k = alignSize >> PAGE_SHIFT;          // 对齐后需要多少页

        PageCache::GetInstance()->GetMtx().lock();
        Span *span = PageCache::GetInstance()->NewSpan(k, SpanState::Direct);
        // 页级分配没有切成小对象，use_count 保持 0。
        // _objSize 记录可用空间大小，realloc 可直接用它作为旧块容量。
        assert(span->_state == SpanState::Direct);
        span->use_count = 0;
        span->_freeList = nullptr;
        span->_objSize = span->_n << PAGE_SHIFT;
        PageCache::GetInstance()->GetMtx().unlock();

        return (void *)(span->_pageID << PAGE_SHIFT);
    }

    return GetThreadCache()->Allocate(size);
}

void tcfree(void *ptr)
{
    // 空指针直接返回
    if (ptr == nullptr)
    {
        return;
    }

    Span *span = PageCache::GetInstance()->MapObjectToSpan(ptr);
    if (span->_state == SpanState::Small)
    {
        GetThreadCache()->Deallocate(ptr, span->_objSize);
    }
    else if (span->_state == SpanState::Direct)
    {
        // 本阶段不做 Direct Span 起始地址完整校验。
        PageCache::GetInstance()->GetMtx().lock();
        PageCache::GetInstance()->ReleaseSpanToPageCache(span);
        PageCache::GetInstance()->GetMtx().unlock();
    }
    else
    {
        assert(false && "invalid span state for free");
        abort();
    }
}
