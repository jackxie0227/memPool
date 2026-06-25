#include "ConcurrentAlloc.h"

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
        Span *span = PageCache::GetInstance()->NewSpan(k);
        // 页级分配没有切成小对象，use_count 保持 0。
        // _objSize 记录可用空间大小，realloc 可直接用它作为旧块容量。
        span->_objSize = span->_n << PAGE_SHIFT;
        PageCache::GetInstance()->GetMtx().unlock();

        return (void *)(span->_pageID << PAGE_SHIFT);
    }

    return GetThreadCache()->Allocate(size);
}

void tcfree(void *ptr)
{
    if (ptr == nullptr)
    {
        return;
    }

    Span *span = PageCache::GetInstance()->MapObjectToSpan(ptr);
    if (span->use_count == 0)
    {
        // use_count == 0 表示这是直接从 PageCache 分配的整页 Span，
        // 包括大对象和大 alignment 分配。用户指针就是 Span 起点。
        PageCache::GetInstance()->GetMtx().lock();
        PageCache::GetInstance()->ReleaseSpanToPageCache(span);
        PageCache::GetInstance()->GetMtx().unlock();
    }
    else
    {
        GetThreadCache()->Deallocate(ptr, span->_objSize);
    }
}
