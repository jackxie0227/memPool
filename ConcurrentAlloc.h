#pragma once
#include "common.h"
#include "PageCache.h"
#include "ObjectPool.h"
#include "ThreadCache.h"

// 线程申请空间接口
void *tcmalloc(size_t size)
{
    if (size > MAX_BYTES)
    {                                                // 单词申请空间大于 256KB
        size_t alignSize = SizeClass::RoundUp(size); // 空间对齐
        size_t k = alignSize >> PAGE_SHIFT;          // 对齐后需要多少页 8KB/页

        PageCache::GetInstance()->GetMtx().lock(); // 需要对 PageCache 中的 span 操作，上锁
        Span *span = PageCache::GetInstance()->NewSpan(k);
        span->_objSize = size;
        PageCache::GetInstance()->GetMtx().unlock();

        void *ptr = (void *)(span->_pageID << PAGE_SHIFT);
        return ptr;
    }
    else
    { // 单次申请空间小于256KB
        if (pTLSThreadCache == nullptr)
        {
            static ObjectPool<ThreadCache> objPool;
            objPool.getMtx().lock();
            pTLSThreadCache = objPool.New(); // 申请空间
            objPool.getMtx().unlock();
        }
        return pTLSThreadCache->Allocate(size);
    }
}

// 线程回收空间接口
// ptr - 回收空间地址
// size - 回收空间字节数
void tcfree(void *ptr)
{
    assert(ptr);
    Span *span = PageCache::GetInstance()->MapObjectToSpan(ptr);
    size_t size = span->_objSize;
    if (size > MAX_BYTES)
    {
        PageCache::GetInstance()->GetMtx().lock();
        PageCache::GetInstance()->ReleaseSpanToPageCache(span);
        PageCache::GetInstance()->GetMtx().unlock();
    }
    else
        pTLSThreadCache->Deallocate(ptr, size);
}