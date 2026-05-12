#pragma once
#ifdef _WIN32
#include <Windows.h>
#else
// Linux相关头文件
#include <sys/mman.h>
#include <unistd.h>
#endif
#include <iostream>
#include <unordered_map>
#include <vector>
#include <assert.h>
#include <thread>
#include <mutex>
#include <algorithm>
using std::cout;
using std::endl;

static const size_t FREE_LIST_NUM = 208;    // 哈希表中自由链表个数
static const size_t MAX_BYTES = 256 * 1024; // ThreadCache 单次申请的最大字节数
static const size_t PAGE_NUM = 129;         // span的最大管理页数
#ifdef _WIN32
static const size_t PAGE_SHIFT = 13; // 一页13位 - 8KB
#else
static const size_t PAGE_SHIFT = 12;
#endif

/* 返回指向传入地址指针字节数的指针 */
/* 自动适配系统位数 64位-8bit 32位-4bit */
static void *&ObjNext(void *obj)
{                         // 后续需要修改，返回引用
    return *(void **)obj; // 临时创建的指针 需要返回引用
}

class FreeList
{
public:
    size_t Size()
    {
        return _size;
    }
    // 向自由链表中头插多块空间
    void PushRange(void *start, void *end, size_t size)
    {
        ObjNext(end) = _freeList; // 使 end 部分指针指向当前自由链表头节点
        _freeList = start;        // 自由链表头节点直接指向多块空间的起始地址
        _size += size;
    }
    // 删除桶中n个块
    void PopRange(void *&start, void *&end, size_t n)
    {
        // 删除块数不能超过桶中仍有块数
        assert(n <= _size);
        start = end = _freeList;
        for (size_t i = 0; i < n - 1; ++i)
        {
            end = ObjNext(end);
        }
        _freeList = ObjNext(end);
        ObjNext(end) = nullptr;
        _size -= n;
    }
    void Push(void *obj)
    {
        // 头插
        assert(obj); // 当obj为空时报错
        ObjNext(obj) = _freeList;
        _freeList = obj;
        ++_size;
    }
    void *Pop()
    {
        assert(_freeList); // 需要有空间才返回空间
        void *obj = _freeList;
        _freeList = ObjNext(obj);
        --_size;

        return obj;
    }
    bool Empty()
    { // 判断哈希桶是否为空
        return _freeList == nullptr;
    }
    size_t &MaxSize()
    { // FreeList未达上限时，能够申请的最大块空间是多少
        // 返回值为引用
        // 方便每次申请成功后让 _maxSize++ 从而进行下一次判断
        return _maxSize;
    }

private:
    void *_freeList = nullptr; // 自由链表 初始为空

    size_t _maxSize = 1; // 当前自由链表申请未达上限时，能够申请的最大块空间是多少
                         // 初始值为1，表示第一次能申请的块数只有1块
                         // 到达上限后 _maxSize值作废

    size_t _size = 0; // 当前自由链表中有多少块空间
                      // 初始值为0 表示开始时链表中没有块
};

class SizeClass
{
public:
    // 计算对齐字节数
    static size_t RoundUp(size_t size)
    {
        if (size <= 128)
        { // [1, 128] 8B对齐
            return _RoundUp(size, 8);
        }
        else if (size <= 1024)
        { // [128 + 1, 1024] 16B对齐
            return _RoundUp(size, 16);
        }
        else if (size <= 8 * 1024)
        { // [1024 + 1, 8*1024] 128B对齐
            return _RoundUp(size, 128);
        }
        else if (size <= 64 * 1024)
        { // [8*1024+1, 64*1024] 1024B对齐
            return _RoundUp(size, 1024);
        }
        else if (size <= 256 * 1024)
        { // [64*1024+1, 256*1024] 8*1024B对齐
            return _RoundUp(size, 8 * 1024);
        }
        else
        { // 申请空间大于256KB，直接按照页来对齐（8KB）
            return _RoundUp(size, 1 << PAGE_SHIFT);
        }
    }
    // 计算每个分区对应的对齐后的字节数
    /*static size_t _RoundUp(size_t size, size_t alignNum) {
        size_t res = 0;
        if (size % alignNum != 0) {
            res = (size/alignNum)*(alignNum+1);
        }
        else res = size;
        return res;
    }*/
    static size_t _RoundUp(size_t size, size_t alignNum)
    {
        // size = 1 => size + alignNum - 1 = 8 => 5'b01000
        // alignNum = 8 => alignNum - 1 = 7 => 5'b00111 => 5'b11000
        // (size + alignNum - 1) & ~(alignNum - 1) = 5'b01000
        return ((size + alignNum - 1) & ~(alignNum - 1));
    }

    // 求size对应在哈希表中的下标
    static inline size_t _Index(size_t size, size_t align_shift)
    {
        // size + 1<<align_shift - 1 可保证不大于下一个对齐数
        // 1<<align_shift = 对齐字节数
        return ((size + (1 << align_shift) - 1) >> align_shift) - 1;
    }
    static inline size_t Index(size_t size)
    {
        assert(size <= MAX_BYTES);

        static int group_array[4] = {16, 56, 56, 56}; // 代表每段哈希桶内链表个数
        if (size <= 128)
        { // [1, 128] 8B --> 2^3B --> align_shift = 3
            return _Index(size, 3);
        }
        else if (size <= 1024)
        {
            return _Index(size - 128, 4) + group_array[0];
        }
        else if (size <= 8 * 1024)
        {
            return _Index(size - 1024, 7) + group_array[1] + group_array[0];
        }
        else if (size <= 64 * 1024)
        {
            return _Index(size - 8 * 1024, 10) + group_array[2] + group_array[1] + group_array[0];
        }
        else if (size <= 256 * 1024)
        {
            return _Index(size - 64 * 1024, 13) + group_array[3] + group_array[2] + group_array[1] + group_array[0];
        }
        else
        {
            assert(false);
        }
        return -1;
    }

    static size_t NumMoveSize(size_t size)
    {                     // 获取单次申请上限
        assert(size > 0); // 不能申请0及以下大小的空间

        // MAX_BYTES 为单个块的最大空间 - 256KB
        int num = MAX_BYTES / size;

        if (num > 512)
        { // 单次申请空间小于 512B
            // 单次申请空间很小时，数量上限原则上非常大
            // 限制在 512
            num = 512;
        }
        else if (num < 2)
        { // 单次申请空间大于 128KB
            // 单次申请空间较大，数量上限等于一
            // 稍稍增大数量上限，提高效率
            num = 2;
        }

        return num;
    }

    static size_t NumMovePage(size_t size)
    { // size 表示一块大小
        // 当CentralCached中没有span为ThreadCached提供小块空间时，CentralCached需要向PageCached申请一块span
        // 此时需要根据一块空间的大小来匹配 保证span为size后尽量不浪费或不足够再频繁申请相同大小的span

        // NumMoveSize是计算出的ThreadCached向CentralCached申请size大小的块时的单词最大申请块数
        size_t num = NumMoveSize(size);

        // num * size 即单词申请最大空间大小
        size_t npage = num * size;

        // PAGE_SHIFT 表示一页要占用多少位 一页8KB为13位
        npage >>= PAGE_SHIFT;

        // 计算结果为0
        if (npage == 0)
            npage = 1;

        return npage;
    }
};

// CentralCache 向操作系统申请内存时，页号类型
// size_t 大小随平台位数变化 32位-4byte 64位-8byte
typedef size_t PageID;
struct Span
{
public:
    PageID _pageID = 0;        // 页号
    size_t _n = 0;             // 当前 span 管理的页数
    void *_freeList = nullptr; // 每个 span 下挂的小块空间的头节点
    size_t use_count = 0;      // 当前 span 被分配出去的小块空间数
    Span *_next = nullptr;     // 双向链表指针
    Span *_prev = nullptr;

    bool _isUse = false;
    size_t _objSize = 0; // span管理页被切分成的块有多大
};

class SpanList
{
public:
    SpanList()
    {
        // 构造函数中哨兵位头节点
        _head = new Span;

        // 双向循环 都指向 _head
        _head->_next = _head;
        _head->_prev = _head;
    }
    bool Empty()
    {
        return _head == _head->_next; // 带头双向链表为空时头节点指向自己
    }
    Span *Begin()
    {
        return _head->_next;
    }
    Span *End()
    {
        return _head;
    }
    Span *PopFront()
    { // 删除首个 span 并返回该span
        Span *front = _head->_next;
        Erase(front);
        return front;
    }
    void PushFront(Span *span)
    { // 头插
        Insert(Begin(), span);
    }
    void Insert(Span *pos, Span *ptr)
    {                       // 在pos位置前插入ptr节点
        assert(pos && ptr); // 确保输入节点非空

        Span *prev = pos->_prev;

        prev->_next = ptr;
        ptr->_prev = prev;

        ptr->_next = pos;
        pos->_prev = ptr;
    }
    void Erase(Span *pos)
    {                         // 删除 pos 位置节点
        assert(pos);          // pos 不为空
        assert(pos != _head); // pos 不为哨兵位

        Span *prev = pos->_prev;
        Span *next = pos->_next;
        prev->_next = next;
        next->_prev = prev;

        // pos 节点不需要通过delete释放
        // 交给PageCache回收而不是直接删掉
    }

    // 获取当前Span的桶锁
    std::mutex &getMtx()
    {
        return _mtx;
    }

private:
    Span *_head;
    std::mutex _mtx; // 每个CentralCache中的哈希桶都需要一个桶锁
};

// 向系统申请 kpage * 8KB 的堆内存
inline static void *SystemAlloc(size_t kpage)
{
#ifdef _WIN32
    void *ptr = VirtualAlloc(0, kpage << PAGE_SHIFT, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
    // linux 下使用 mmap 映射匿名私有内存
    void *ptr = mmap(nullptr, kpage << PAGE_SHIFT, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (ptr == nullptr)
        throw std::bad_alloc();
    return ptr;
}

inline static void SystemFree(void *ptr, size_t kpage)
{
#ifdef _WIN32
    VirtualFree(ptr, 0, MEM_RELEASE); // Win 下释放内存无需传入大小
#else
    munmap(ptr, kpage << PAGE_SHIFT); // Linux 下 munmap 需要传入释放的字节数
#endif
}