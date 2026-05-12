#pragma once
#include "common.h"

class ThreadCache
{
public:
    // 线程申请size大小的空间
    void *Allocate(size_t size);

    // 回收线程中大小为size的obj空间
    void Deallocate(void *obj, size_t size);

    // ThreadCache空间不足时向CentralCache申请空间的接口
    void *FetchFromCentralCache(size_t index, size_t alignSize);

    // ThreadCache向CentralCache归还桶中空间
    void ListTooLong(FreeList &list, size_t size);

private:
    /* 整体控制在最多10%左右内碎片浪费 */
    // size范围    ---           对齐数    ---         对应哈希桶下标范围
    // [1, 128]                 8B      对齐           freelist[0, 16)
    // [128+1, 1024]            16B     对齐           freelist[16, 71)
    // [1024+1, 8*1024]         128B    对齐           freelist[72, 128)
    // [8*1024+1, 64*1024]      1024B   对齐           freelist[128, 184)
    // [64*1024+1, 256*1024]    8*1024B 对齐           freelist[184, 208)
    FreeList _freeLists[FREE_LIST_NUM]; // 哈希桶 每个桶表示一个自由链表
};

// TLS全局对象指针 每个线程都有一个独立的全局对象
static thread_local ThreadCache *pTLSThreadCache = nullptr;
// 注意需要static修饰否则多个文件包含时报错
