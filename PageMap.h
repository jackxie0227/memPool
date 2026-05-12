#pragma once
#include "common.h"
#include <cstdint>
#include <cstring>
#include <cassert>

template <int BITS> // BITS - 存储所有页号至少需要多少位
class TCMalloc_PageMap1
{
private:
    static const int LENGTH = 1 << BITS; // 表长 - 2^BITS 每个页号对应一个槽
    void **array_;

public:
    typedef uintptr_t Number; // 能装下指针位宽的页号类型

    explicit TCMalloc_PageMap1()
    {
        size_t size = sizeof(void *) << BITS;                          // 需要的总字节数 - sizeof(void*)为本平台指针所占用字节数
        size_t alignSize = SizeClass::_RoundUp(size, 1 << PAGE_SHIFT); // 占用多少页 - 1页8KB
        array_ = (void **)SystemAlloc(alignSize >> PAGE_SHIFT);        // 按照页申请系统内存
        memset(array_, 0, sizeof(void *) << BITS);                     // 将实际用到的 size 置零
    }

    void *get(Number k) const
    {
        if ((k >> BITS) > 0)
        {
            return nullptr;
        }
        return array_[k];
    }

    void set(Number k, void *v)
    {
        array_[k] = v;
    }
};

template <int BITS>
class TCMalloc_PageMap2
{
private:
    // Put 32 entries in the root and (2^BITS)/32 entries in each leaf.
    static const int ROOT_BITS = 5; // 32位下前5位搞一个第一层的数组
    static const int ROOT_LENGTH = 1 << ROOT_BITS;

    static const int LEAF_BITS = BITS - ROOT_BITS; // 32位下后14位搞成第二层的数组
    static const int LEAF_LENGTH = 1 << LEAF_BITS;

    // Leaf node
    struct Leaf
    { // 叶子就是后14位的数组
        void *values[LEAF_LENGTH];
    };

    Leaf *root_[ROOT_LENGTH]; // 根就是前5位的数组
public:
    typedef uintptr_t Number;

    // explicit TCMalloc_PageMap2(void* (*allocator)(size_t)) {
    explicit TCMalloc_PageMap2()
    {                                    // 直接把所有的空间都开好
        memset(root_, 0, sizeof(root_)); // 根数组清零 初始化为未分配叶子
        PreallocateMoreMemory();         // 直接开2M的span*全开出来
    }

    void *get(Number k) const
    {
        const Number i1 = k >> LEAF_BITS;        // 取高位索引
        const Number i2 = k & (LEAF_LENGTH - 1); // 取地位索引
        if ((k >> BITS) > 0 || root_[i1] == NULL)
        {
            // k 超过 BITS 的表示范围 or 根槽未分配叶子
            return NULL;
        }
        return root_[i1]->values[i2];
    }

    void set(Number k, void *v)
    {
        const Number i1 = k >> LEAF_BITS;
        const Number i2 = k & (LEAF_LENGTH - 1);
        assert(i1 < ROOT_LENGTH); // 确保根索引不越界
        // root_[i1]->values[i2] = v;

        // 采用懒分配时需要判空并创建 Ensure
        if (root_[i1] == nullptr)
        {
            static ObjectPool<Leaf> leafPool;
            Leaf *leaf = (Leaf *)leafPool.New();
            memset(leaf, 0, sizeof(*leaf));
            root_[i1] = leaf;
        }
        root_[i1]->values[i2] = v;
    }

    // 确保从start开始往后的n页空间开好了
    bool Ensure(Number start, size_t n)
    {
        for (Number key = start; key <= start + n - 1;)
        {
            const Number i1 = key >> LEAF_BITS; // 获取当前key所在叶子编号

            // Check for overflow
            if (i1 >= ROOT_LENGTH)
                return false;

            // 如果没开好就开空间
            if (root_[i1] == NULL)
            {
                static ObjectPool<Leaf> leafPool;
                Leaf *leaf = (Leaf *)leafPool.New();

                memset(leaf, 0, sizeof(*leaf));
                root_[i1] = leaf;
            }

            // 跳转至下一个叶子节点边界
            key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
        }
        return true;
    }

    // 提前开好空间，这里就把2M的直接开好
    void PreallocateMoreMemory()
    {
        // Allocate enough to keep track of all possible pages
        Ensure(0, 1 << BITS);
    }
};

// 三层基数树
template <int BITS>
class TCMalloc_PageMap3
{
private:
    // How many bits should we consume at each interior level
    static const int INTERIOR_BITS = (BITS + 2) / 3; // 向上取整 尽量均分为3份
    static const int INTERIOR_LENGTH = 1 << INTERIOR_BITS;
    // How many bits should we consume at leaf level
    static const int LEAF_BITS = BITS - 2 * INTERIOR_BITS; // 叶子节点使用剩余的位数
    static const int LEAF_LENGTH = 1 << LEAF_BITS;
    // Interior node
    struct Node
    {
        Node *ptrs[INTERIOR_LENGTH];
    };
    // Leaf node
    struct Leaf
    {
        void *values[LEAF_LENGTH];
    };
    Node *root_; // 根节点指针
    // void* (*allocator_)(size_t);           // 外部内存分配函数 void* func(size_t size);
    Node *NewNode()
    { // 申请一个内部节点并清零
        static ObjectPool<Node> NodePool;
        Node *result = (Node *)NodePool.New();
        // Node* result = reinterpret_cast<Node*>((*allocator_)(sizeof(Node)));
        if (result != NULL)
        {
            memset(result, 0, sizeof(*result)); // 所有槽置空
        }
        return result;
    }

public:
    typedef uintptr_t Number;
    explicit TCMalloc_PageMap3()
    {
        // allocator_ = allocator;
        root_ = NewNode();
    }
    void *get(Number k) const
    {
        const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
        const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);
        if ((k >> BITS) > 0 || // 越界 or 中间节点缺失
            root_->ptrs[i1] == NULL || root_->ptrs[i1]->ptrs[i2] == NULL)
        {
            return NULL;
        }
        return reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2])->values[i3];
    }
    void set(Number k, void *v)
    {
        assert(k >> BITS == 0);
        assert(Ensure(k, 1)); // 确保位于k的键已经分配空间

        const Number i1 = k >> (LEAF_BITS + INTERIOR_BITS);
        const Number i2 = (k >> LEAF_BITS) & (INTERIOR_LENGTH - 1);
        const Number i3 = k & (LEAF_LENGTH - 1);

        Leaf *leaf = reinterpret_cast<Leaf *>(root_->ptrs[i1]->ptrs[i2]);
        leaf->values[i3] = v;
    }
    bool Ensure(Number start, size_t n)
    { // 确保n页对应的每层数组的空间大小已经开辟完毕
        for (Number key = start; key <= start + n - 1;)
        {
            const Number i1 = key >> (LEAF_BITS + INTERIOR_BITS);         // 根索引
            const Number i2 = (key >> LEAF_BITS) & (INTERIOR_LENGTH - 1); // 中间层索引
            // Check for overflow
            if (i1 >= INTERIOR_LENGTH || i2 >= INTERIOR_LENGTH) // 越界检查
                return false;
            // 根数组中对应位置节点不存在 - 中间层数组缺失
            if (root_->ptrs[i1] == NULL)
            {
                Node *n = NewNode();
                if (n == NULL)
                    return false;
                root_->ptrs[i1] = n;
            }
            // 中间层数组中对应位置节点不存在 - 叶节点数组缺失
            if (root_->ptrs[i1]->ptrs[i2] == NULL)
            {
                static ObjectPool<Leaf> LeafPool;
                Leaf *leaf = LeafPool.New();
                if (leaf == NULL)
                    return false;
                memset(leaf, 0, sizeof(*leaf));
                root_->ptrs[i1]->ptrs[i2] = reinterpret_cast<Node *>(leaf);
            }
            // 跳转至下一个叶子
            key = ((key >> LEAF_BITS) + 1) << LEAF_BITS;
        }
        return true;
    }
};
