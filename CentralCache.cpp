#include "CentralCache.h"
#include "PageCache.h"

size_t CentralCache::FetchRangeObj(void *&start, void *&end, size_t batchNum, size_t size)
{
    // 获取到size对应哪一个SpanList
    size_t index = SizeClass::Index(size);
    // 此时 _spanLists[index] 位置所挂的 span 情况可分为3种
    // 1. 有span且span所管理的空间不为空 - 直接获取非空的span即可
    // 2. 有span但span所管理的空间为空 - 向 PageCache 申请一个新的span
    // 3. 没有span - 向 PageCache 申请一个新的span

    // 整个进程中只有一个 CentralCache
    // 可能存在多个线程同时像一个 CentralCache 申请span中的块空间的情况
    // 需要对SpanList的操作加锁
    /***************上锁线程操作***************/
    _spanLists[index].getMtx().lock(); // 给对应桶加锁
    Span *span = GetOneSpan(_spanLists[index], size);
    assert(span);
    assert(span->_freeList); // span 和 span 的管理空间不能为空
    end = start = span->_freeList;
    size_t actualNum = 1; // 函数实际返回值
    size_t i = 0;
    while (i < batchNum - 1 && ObjNext(end) != nullptr)
    {
        end = ObjNext(end); // 通过 ObjNext(end) 可直接指向自由链表中 end 的下一个节点
        i++;
        actualNum++;
    }
    span->_freeList = ObjNext(end);      // 将span指向的自由链表头节点设置为end后面的节点
    span->use_count += actualNum;        // 给 ThreadCache 分了多少就加多少
    ObjNext(end) = nullptr;              // 将end指向的下一个节点设置为空
    _spanLists[index].getMtx().unlock(); // 解锁
    /*****************************************/

    return actualNum;
}

// 获取一个管理空间非空的Span
Span *CentralCache::GetOneSpan(SpanList &list, size_t size)
{
    // 让 CentralCache拿到一个管理空间非空的span 可能有也可能没有
    // 首先判断 CentralCache 对应index下挂的有没有管理非空空间的span
    // 若有则将该span返回 否则向PageCache申请新的span 直接调用 NewSpan
    Span *it = list.Begin();
    while (it != list.End())
    {
        if (it->_freeList != nullptr) // 找到管理空间非空的span
            return it;
        else // 没找到 继续往下找
            it = it->_next;
    }

    // 该桶中没有span 解锁
    list.getMtx().unlock();

    // 此时已确认 CentralCache 中没有管理空间非空的 span
    // 需要向 PageCache 申请全新的 span - 调用 NewSpan
    // NewSpan需传入要申请的页数，这样PageCache才能根据映射关系从对应下标处找到对应span
    // 此时需要使用块页匹配算法将传入的size转换为页数
    size_t k = SizeClass::NumMovePage(size);
    // 调用NewSpan获取一个全新span
    PageCache::GetInstance()->GetMtx().lock();
    Span *span = PageCache::GetInstance()->NewSpan(k);
    span->_objSize = size;
    // span->_isUse = true; // 标记该span已修改
    PageCache::GetInstance()->GetMtx().unlock();
    // 此时旧从PageCache中获取到一个 没有划分过 的全新span
    // 此时需要根据size划分该获取的span
    char *start = (char *)(span->_pageID << PAGE_SHIFT);    // 这里的_pageID 与 pc中的128页没关系  只代表内存中的页id
    char *end = (char *)(start + (span->_n << PAGE_SHIFT)); // 虚拟地址

    // 开始切分span管理的空间
    span->_freeList = start; // 管理空间放到span
    void *tail = start;
    start += size;
    while (start + size < end)
    { // size不一定能够被_n*8KB整除 start<end防止越界
        ObjNext(tail) = start;
        start += size;
        tail = ObjNext(tail);
    }
    ObjNext(tail) = nullptr;
    // 切分好span后，将span挂到CentralCache对应下标的桶中
    // 在新的span挂在桶之前，其它线程同时申请该桶时不会产生竞争问题
    list.getMtx().lock();
    list.PushFront(span);

    return span;
}

void CentralCache::ReleaseListToSpans(void *start, size_t size)
{
    // 通过 size 找到对应的桶在哪里
    size_t index = SizeClass::Index(size);

    _spanLists[index].getMtx().lock(); // 对桶操作，上锁

    // 依次遍历 start 将各个块放到对应页的span所管理的_freeList中
    while (start)
    {
        void *next = ObjNext(start);
        Span *span = PageCache::GetInstance()->MapObjectToSpan(start); // 哈希表获取对应span

        // 将 start 为首的内存块头插至 span 的自由链表中
        ObjNext(start) = span->_freeList;
        span->_freeList = start;

        // 记录span中已使用的块个数
        span->use_count--;
        if (span->use_count == 0)
        {
            // 在 CentralCache 中删除span
            _spanLists[index].Erase(span);
            span->_freeList = nullptr; // 归还时顺序被打乱 _freeList无用  _pageID和_n
            span->_prev = nullptr;
            span->_next = nullptr;
            // 归还span，解掉当前桶锁
            _spanLists[index].getMtx().unlock();

            // 这个span需要交给PageCache管理 需加锁
            PageCache::GetInstance()->GetMtx().lock();
            PageCache::GetInstance()->ReleaseSpanToPageCache(span);
            PageCache::GetInstance()->GetMtx().unlock();

            // 归还完毕，加上当前桶锁
            _spanLists[index].getMtx().lock();
        }
        start = next;
    }

    _spanLists[index].getMtx().unlock(); // 操作完成，解锁
}
