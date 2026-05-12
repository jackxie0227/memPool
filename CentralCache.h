#pragma once
#include "common.h"

class CentralCache
{
public:
    // 单例接口
    static CentralCache *GetInstance()
    {
        return &_sInst;
    }

    // CentralCache 从自己的 _spanLists 中为 ThreadCache 提供所需的块空间
    size_t FetchRangeObj(void *&start, void *&end, size_t n, size_t size);
    // start end 表示 CentralCache 提供的空间的开始结尾 输出型参数 使用引用
    // n 表示 ThreadCache 需要多少块大小为 size 的空间
    // size 表示 ThreadCache 所需的单块空间的块大小
    // 返回值为 CentralCache 实际提供的空间大小

    // 获取一个管理空间不为空的span
    Span *GetOneSpan(SpanList &list, size_t size);

    // 将ThreadCache多出来的内存块还到span中
    void ReleaseListToSpans(void *start, size_t size);

private:
    CentralCache() {}
    CentralCache(const CentralCache &copy) = delete;
    CentralCache &operator=(const CentralCache &copy) = delete;
    SpanList _spanLists[FREE_LIST_NUM]; // 哈希桶中挂的是一个个Span
    static CentralCache _sInst;         // 饿汉模式创建一个全局单例 CentralCache
};