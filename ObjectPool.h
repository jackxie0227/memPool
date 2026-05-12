#pragma once
#include "common.h"

template <class T>
class ObjectPool
{
public:
    T *New()
    {                     // 申请一个T类型大小的空间
        T *obj = nullptr; // 最终返回的空间

        if (_freelist)
        { // 存在可利用的旧空间
            void *next = *(void **)_freelist;
            obj = (T *)_freelist;
            _freelist = next;
        }
        else
        { // 无可利用的旧空间
            //_memory中剩余空间小于T的类型大小时再向内存申请新的空间
            if (_remanentBytes < sizeof(T))
            {
                _remanentBytes = 128 * 1024; // 新开辟 128K 的空间
                // _memory = (char*)malloc(_remanentBytes);
                _memory = (char *)SystemAlloc(_remanentBytes >> PAGE_SHIFT); // 右移13位 = 除以2^13 = 需要申请的页数
                if (_memory == nullptr)
                { // 内存不足时malloc返回空指针，抛出异常
                    throw std::bad_alloc();
                }
            }

            obj = (T *)_memory;                                                       // 给定一个T类型大小
            size_t objSize = sizeof(T) < sizeof(void *) ? sizeof(void *) : sizeof(T); // 若小于指针大小则固定返回一个指针类型大小的空间 8Byte
            _memory += objSize;                                                       // _memory 后移一个T类型大小
            _remanentBytes -= objSize;                                                // 剩余空间类型大小减去当前所创建的类型T的大小
        }
        new (obj) T; // 使用定位new调用构造函数进行初始化
        return obj;
    }

    void Delete(T *obj)
    { // 回收还回来的小空间
        // 显式调用析构函数
        obj->~T();

        // 头插
        *(void **)obj = _freelist; // 指向旧的内存块 初始时为nullptr
        _freelist = obj;           // 头指针指向新块
    }

    std::mutex &getMtx()
    {
        return _poolMtx;
    }

private:
    // 指向大块内存 - 每次申请一块空间后 _memory 需要往后移动一个模板类型大小
    // void* 无法向后挪动或解引用
    char *_memory = nullptr;

    // 自由链表，用来连接归还的空闲空间 - 先用完的内存块入队 - 头插
    void *_freelist = nullptr;

    // 大块内存再切分过程中的剩余字节数
    size_t _remanentBytes = 0;

    // 防止 ThreadCache 申请时申请到空指针
    std::mutex _poolMtx;
};