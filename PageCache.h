#pragma once
#include "common.h"
#include "ObjectPool.h"
#include "PageMap.h"

class PageCache
{
public:
    // 饿汉单例
    static PageCache *GetInstance()
    {
        static PageCache inst;
        return &inst;
    }

    // PageCache 从 _spanLists 中拿出一个k页的span
    Span *NewSpan(size_t k, SpanState targetState);

    // 返回一个起始地址满足 alignPages 页对齐的 span。
    // 该接口用于 alignment > page_size 的标准对齐分配，返回值本身就是用户指针起点。
    Span *NewAligned(size_t k, size_t alignPages);
    std::mutex &GetMtx()
    {
        return _pageMtx;
    }

    // 通过页地址找到span
    Span *MapObjectToSpan(void *obj);

    // 管理 CentralCache 还回来的 span
    void ReleaseSpanToPageCache(Span *span);

private:
    // PageCache管理128个SpanList
    // 每个Page下存储不同长度的span（span长度与页编号数相同 1-128）
    SpanList _spanLists[PAGE_NUM]; // 双向循环链表
    std::mutex _pageMtx;           // PageCache 整体锁
    ObjectPool<Span> _spanPool;

private:
    PageCache() {} // 构造函数私有防止外部申请新的 PageCache 确保唯一性
    PageCache(const PageCache &pc) = delete;
    PageCache &operator=(const PageCache &pc) = delete;
private:
    // 哈希映射 用于快速通过页号找到对应span
    // std::unordered_map<PageID, Span*> _idSpanMap;
    // TCMalloc_PageMap1<32 - PAGE_SHIFT> _idSpanMap;
    TCMalloc_PageMap3<48 - PAGE_SHIFT> _idSpanMap;
};
