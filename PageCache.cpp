#include "PageCache.h"

PageCache PageCache::_sInst; // 单例对象

Span *PageCache::NewSpan(size_t k)
{
    // 申请页数一定在[1, PAGE_NUM - 1] 范围内
    assert(k > 0);

    if (k >= PAGE_NUM)
    {
        void *ptr = SystemAlloc(k);
        Span *bigSpan = _spanPool.New();
        bigSpan->_pageID = ((PageID)ptr) >> PAGE_SHIFT;
        bigSpan->_n = k;
        // _idSpanMap[bigSpan->_pageID] = bigSpan;
        _idSpanMap.set(bigSpan->_pageID, bigSpan);
        return bigSpan;
    }

    // 1. k号桶中有span
    if (!_spanLists[k].Empty())
    {
        Span *span = _spanLists[k].PopFront();

        // 记录哈希页号-span* 的哈希映射
        for (PageID i = 0; i < span->_n; ++i)
        {
            // _idSpanMap[span->_pageID + i] = span;
            _idSpanMap.set(span->_pageID + i, span);
        }
        span->_isUse = true; // 标记该块已使用
        return span;
    }

    // 2. k号桶中没有span 但大于k的桶中有span
    for (size_t i = k + 1; i < PAGE_NUM; i++)
    {
        if (!_spanLists[i].Empty())
        {
            // i号桶中有span 对该span进行切分
            Span *nSpan = _spanLists[i].PopFront();
            // 将该span切分为一个k页的和一个 n-k 页的
            // Span* kSpan = new Span;
            Span *kSpan = _spanPool.New();
            kSpan->_pageID = nSpan->_pageID;
            kSpan->_n = k;
            nSpan->_pageID = kSpan->_pageID + k;
            nSpan->_n -= k;
            // 将 n-k 页放回对应哈希桶中
            _spanLists[nSpan->_n].PushFront(nSpan);

            // 将n-k页的span边缘页映射一下，方便后续合并
            // _idSpanMap[nSpan->_pageID] = nSpan;
            // _idSpanMap[nSpan->_pageID + nSpan->_n - 1] = nSpan;
            _idSpanMap.set(nSpan->_pageID, nSpan);
            _idSpanMap.set(nSpan->_pageID + nSpan->_n - 1, nSpan);
            // 映射边缘页后，在合并时可以通过 pageID-1 或 pageID+_n 找到

            // 记录哈希映射关系 页地址-span*
            for (size_t i = 0; i < kSpan->_n; i++)
            {
                // _idSpanMap[kSpan->_pageID + i] = kSpan;
                _idSpanMap.set(kSpan->_pageID + i, kSpan);
            }
            kSpan->_isUse = true; // 标记该span已使用
            return kSpan;
        }
    }

    // 3. k号桶和后面的桶都没有span
    // 直接向系统申请128页的span
    void *ptr = SystemAlloc(PAGE_NUM - 1);
    // 开一个新的span维护这块空间
    Span *bigSpan = _spanPool.New();
    bigSpan->_pageID = ((PageID)ptr) >> PAGE_SHIFT;
    bigSpan->_n = PAGE_NUM - 1;
    // 将该span放到对应哈希桶中
    _spanLists[PAGE_NUM - 1].PushFront(bigSpan);

    return NewSpan(k); // 此时PageCache至少有一个128页的span 递归走②
}

// 通过页地址找span
Span *PageCache::MapObjectToSpan(void *obj)
{
    PageID id = (((PageID)obj) >> PAGE_SHIFT); // 获取页号

    // std::unique_lock<std::mutex> lc(_pageMtx);

    // 通过哈希找到页号对应的span
    // auto ret = _idSpanMap.find(id);
    // if (ret != _idSpanMap.end()) { // 成功找到
    //	return ret->second;
    //}

    Span *ret = (Span *)_idSpanMap.get(id);
    if (ret != nullptr)
    {
        return ret;
    }

    assert(false);
    return nullptr;
}

// 管理 CentralCache 还回来的span
void PageCache::ReleaseSpanToPageCache(Span *span)
{
    // 通过span判断释放空间页数是否大于128页
    // 大于128页时直接还给os
    if (span->_n > PAGE_NUM - 1)
    {
        void *ptr = (void *)(span->_pageID << PAGE_SHIFT);
        SystemFree(ptr, span->_n); // 直接调用系统接口释放空间
        _spanPool.Delete(span);
        // delete span;
        return;
    }

    // 向左不断合并
    while (1)
    {
        PageID leftID = span->_pageID - 1; // 拿到左边相邻页
        // auto ret = _idSpanMap.find(leftID);
        Span *ret = (Span *)_idSpanMap.get(leftID);

        // 没有相邻span，停止合并
        /*if (ret == _idSpanMap.end()) {
            break;
        }*/
        if (ret == nullptr)
            break;

        // Span* leftSpan = ret->second; // 获取相邻span
        Span *leftSpan = ret;
        // 相邻span在cc中，停止合并
        if (leftSpan->_isUse)
        {
            break;
        }

        // 相邻span与当期span合并后超过128页，停止合并
        if (leftSpan->_n + span->_n > PAGE_NUM - 1)
        {
            break;
        }

        // 当前span与相邻span进行合并
        span->_pageID = leftSpan->_pageID;
        span->_n += leftSpan->_n;

        _spanLists[leftSpan->_n].Erase(leftSpan); // 将相邻span对象从桶中删除
        // delete leftSpan;
        _spanPool.Delete(leftSpan);
    }

    // 向右不断合并
    while (1)
    {
        PageID rightID = span->_pageID + span->_n;
        // auto it = _idSpanMap.find(rightID);
        Span *it = (Span *)_idSpanMap.get(rightID);

        // 没有相邻span，停止合并
        /*if (it == _idSpanMap.end()) {
            break;
        }*/
        if (it == nullptr)
            break;

        Span *rightSpan = it;
        // Span* rightSpan = it;
        // 相邻span在cc中，停止合并
        if (rightSpan->_isUse)
        {
            break;
        }

        // 相邻span与当期span合并后超过128页，停止合并
        if (rightSpan->_n + span->_n > PAGE_NUM - 1)
        {
            break;
        }

        // 当前span与相邻span进行合并
        // 此时rightSpan直接拼在span后面 无需修改span->pageID
        span->_n += rightSpan->_n;
        _spanLists[rightSpan->_n].Erase(rightSpan);
        // delete rightSpan;
        _spanPool.Delete(rightSpan);
    }

    // 合并完毕，将当前span挂到对应桶中
    _spanLists[span->_n].PushFront(span);
    span->_isUse = false; // 取消已使用标记

    // 映射当前span的边缘页 后续还可以对该span进行合并
    // _idSpanMap[span->_pageID] = span;
    // _idSpanMap[span->_pageID + span->_n - 1] = span;
    _idSpanMap.set(span->_pageID, span);
    _idSpanMap.set(span->_pageID + span->_n - 1, span);
}
