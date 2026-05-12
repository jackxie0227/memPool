#include "ThreadCache.h"
#include "CentralCache.h"

void *ThreadCache::Allocate(size_t size)
{
    // 确保单次申请不超过 256KB 空间
    assert(size <= MAX_BYTES);

    // 获取size对应的 哈希桶下标-index  对齐后的字节数-alignSize
    size_t alignSize = SizeClass::RoundUp(size);
    size_t index = SizeClass::Index(size);

    if (!_freeLists[index].Empty())
    { // 对应下标链表不为空
        return _freeLists[index].Pop();
    }
    else
    {                                                   // 对应下标链表为空 需要让 ThreadCache 向 CentralCache 申请空间
        return FetchFromCentralCache(index, alignSize); // 申请空间时直接申请对齐后的大小
    }
}

void ThreadCache::Deallocate(void *obj, size_t size)
{
    assert(obj);
    assert(size < MAX_BYTES);

    // 找到 size 对应的自由链表
    size_t index = SizeClass::Index(size);

    // 用自由链表回收空间
    _freeLists[index].Push(obj);

    // 当前桶中的块数大于等于单批次申请块数的时候归还空间
    if (_freeLists[index].Size() >= _freeLists[index].MaxSize())
    {
        ListTooLong(_freeLists[index], size);
    }
}

// ThreadCache中空间不够时，向CentralCache申请空间的接口
void *ThreadCache::FetchFromCentralCache(size_t index, size_t alignSize)
{
    size_t batchNum = std::min(_freeLists[index].MaxSize(), SizeClass::NumMoveSize(alignSize));
    // MaxSize表示index位置的自由链表单次申请未达上限时，能够申请的最大块空间是多少
    // NumMoveSize表示ThreadCache单次向CentralCache申请alignSize大小的空间块的最多块数是多少
    // 二者取小得到本次要给ThreadCache提供多少块alignSize大小的空间
    // alignSize-8B MaxSize-1 NumMoveSize-512 最终返回给ThreadCache一块8B的空间

    if (batchNum == _freeLists[index].MaxSize())
    {
        // 已经达到自由链表申请数量上限
        _freeLists[index].MaxSize()++;
        // 慢开始反馈调节
        // 每次达到上限就使上限++
    }

    // 至此确定内容如下：
    // 从 CentralCache 的 index 下标的哈希桶中拿出 batchNum 块大小为 alignSize 的块空间
    // 接下来需要从对应 index 下标的 SpanList 中挑出一个 Span，然后从Span中挑出大小为 batchNum * alignSize 的一段空间
    void *start = nullptr;
    void *end = nullptr;

    // 返回值 actualNum 为实际获取到的块数
    size_t actualNum = CentralCache::GetInstance()->FetchRangeObj(start, end, batchNum, alignSize);

    assert(actualNum >= 1); // actualNum 始终保持大于等于1

    if (actualNum == 1)
    {
        // actualNum只有大于1时才有多的内存块给线程存储在自由链表中
        // 如果actualNum等于1，直接将start返回给线程
        assert(start == end); // 确保此时actualNum==1
        return start;
    }
    else
    {
        // 如果actualNum大于1，就要给ThreadCache对应index的哈希桶插入
        _freeLists[index].PushRange(ObjNext(start), end, actualNum - 1);
        return start;
    }
}

void ThreadCache::ListTooLong(FreeList &list, size_t size)
{
    void *start = nullptr;
    void *end = nullptr;

    list.PopRange(start, end, list.MaxSize());
    // 此时从list中头部删除了MaxSize块内存

    // 将空间回收至 CentralCache
    CentralCache::GetInstance()->ReleaseListToSpans(start, size);
}
