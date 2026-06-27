# 链接式 malloc/new 替换与 alignment 策略说明

本文档记录本次从原始版本重新改造内存池的内容。目标是生成 `libmempool.so`，让应用通过链接或 `LD_PRELOAD` 即可把 `malloc/free` 与 `new/delete` 替换到底层内存池。

## 改造目标

原始版本需要业务代码包含 `ConcurrentAlloc.h`，并显式调用：

```cpp
void* p = tcmalloc(size);
tcfree(p);
```

改造后可以使用标准接口：

```cpp
void* p = malloc(size);
free(p);

auto obj = new SomeType;
delete obj;
```

也可以用运行时预加载：

```bash
LD_PRELOAD=/home/ubuntu/memPool/build/libmempool.so ./app
```

## 与上一版 header 包装法的区别

上一版标准接口层会在用户指针前放 `AllocationHeader`，并允许返回给用户的地址是底层块内部的某个偏移地址：

```text
raw ... padding ... [header][user memory]
```

这能快速支持任意 alignment，但每次标准分配都有 header 开销，并且 `free(user)` 必须先通过 header 找回真实 `raw`。

本次改为更接近 gperftools release 版的策略：

```text
返回给用户的指针始终是内存池块起点或 Span 起点
```

因此 `free/delete` 可以直接调用 `tcfree(ptr)`，通过页号映射找到 `Span`，不需要每个用户指针前的 header。

## alignment 处理策略

### 普通 malloc/new

普通 `malloc/new` 需要满足默认对齐要求。当前项目将第一档桶从 8 字节对齐改为 16 字节对齐：

```text
FREE_LIST_NUM: 208 -> 200

[1, 128]              16B 对齐 -> 8 个桶
[129, 1024]           16B 对齐 -> 56 个桶
[1025, 8KB]           128B 对齐 -> 56 个桶
[8KB+1, 64KB]         1024B 对齐 -> 56 个桶
[64KB+1, 256KB]       8KB 对齐 -> 24 个桶
```

`common.h` 中保留静态断言：

```cpp
static const size_t DEFAULT_ALIGN = alignof(std::max_align_t);
static_assert(DEFAULT_ALIGN == 16, "This size class layout expects 16-byte default alignment");
```

### alignment <= page_size

对于 `posix_memalign`、`aligned_alloc`、C++17 aligned new，如果请求的 alignment 不超过页大小，则不在块内部移动用户指针，而是把 size 向上修正到 alignment 的倍数后走普通 `tcmalloc`。

原因是：span 起点页对齐，块大小也是 alignment 的倍数，所以切出来的每个块起点都满足该 alignment。

对应代码入口在 `MallocReplacement.cpp`：

```cpp
void *AllocateAligned(size_t alignment, size_t size) noexcept
{
    if (alignment < DEFAULT_ALIGN) {
        alignment = DEFAULT_ALIGN;
    }

    if (alignment <= PageSize()) {
        size_t roundedSize = SizeClass::_RoundUp(size == 0 ? 1 : size, alignment);
        return AllocateStandard(roundedSize);
    }

    return AllocatePageAligned(alignment, size);
}
```

这一条路径的完整流程是：

```text
posix_memalign/aligned_alloc/aligned new
    -> AllocateAligned(alignment, size)
    -> alignment <= PageSize()
    -> roundedSize = RoundUp(size, alignment)
    -> AllocateStandard(roundedSize)
    -> tcmalloc(roundedSize)
    -> ThreadCache/CentralCache/PageCache 正常小对象分配路径
```

举例：用户调用：

```cpp
void* p = nullptr;
posix_memalign(&p, 64, 100);
```

执行过程：

```text
alignment = 64
size = 100
roundedSize = RoundUp(100, 64) = 128
tcmalloc(128)
```

为什么 `tcmalloc(128)` 返回的地址一定满足 64 字节对齐？

1. `CentralCache::GetOneSpan` 从 `PageCache` 拿到的 span 起点是页对齐的。
2. 页大小是 `1 << PAGE_SHIFT`，页起点天然满足 64 字节对齐。
3. 该 span 会按 `size = 128` 切成小块。
4. 128 是 64 的倍数，所以每个块起点仍然满足 64 字节对齐。

内存示意：

```text
span start: 0x1000  (page aligned)

0x1000  block 0, size 128, aligned 64
0x1080  block 1, size 128, aligned 64
0x1100  block 2, size 128, aligned 64
...
```

所以这一类分配不需要额外 header，也不需要把用户指针挪到块内部。返回给用户的 `p` 就是自由链表中的块起点。释放时：

```text
free(p)
    -> tcfree(p)
    -> PageCache::MapObjectToSpan(p)
    -> span->_objSize == 128
    -> ThreadCache::Deallocate(p, 128)
```

这和普通小对象释放流程完全一致。

### alignment > page_size

对于大于页大小的 alignment，新增：

```cpp
Span* PageCache::NewAligned(size_t k, size_t alignPages);
```

它会多申请一些页，找到满足 `alignPages` 对齐的页号，把前导页和尾部页切下来归还 PageCache，最终返回的 Span 起点就是用户指针。

这样避免了 `raw/user` 不一致的问题。

对应代码入口仍然是 `MallocReplacement.cpp` 的 `AllocateAligned`：

```cpp
if (alignment <= PageSize()) {
    ...
}

return AllocatePageAligned(alignment, size);
```

当 alignment 大于页大小时，流程进入：

```cpp
void *AllocatePageAligned(size_t alignment, size_t size) noexcept
{
    size_t k = PagesForBytes(size);
    size_t alignPages = alignment >> PAGE_SHIFT;

    std::lock_guard<std::mutex> lock(PageCache::GetInstance()->GetMtx());
    Span *span = PageCache::GetInstance()->NewAligned(k, alignPages);
    span->_objSize = span->_n << PAGE_SHIFT;
    return (void *)(span->_pageID << PAGE_SHIFT);
}
```

这一条路径的完整流程是：

```text
posix_memalign/aligned_alloc/aligned new
    -> AllocateAligned(alignment, size)
    -> alignment > PageSize()
    -> AllocatePageAligned(alignment, size)
    -> k = PagesForBytes(size)
    -> alignPages = alignment / page_size
    -> PageCache::NewAligned(k, alignPages)
    -> 返回对齐后的 Span 起点
```

举例：假设 `PAGE_SHIFT = 12`，页大小是 4096。用户调用：

```cpp
void* p = nullptr;
posix_memalign(&p, 1 << 20, 100);
```

执行过程：

```text
alignment = 1MB
page_size = 4KB
alignPages = 1MB / 4KB = 256
size = 100
k = PagesForBytes(100) = 1
PageCache::NewAligned(1, 256)
```

`PageCache::NewAligned` 的核心代码：

```cpp
Span *span = NewSpan(k + alignPages);

PageID alignedPageID = SizeClass::_RoundUp(span->_pageID, alignPages);
size_t leading = alignedPageID - span->_pageID;

if (leading > 0) {
    // 前导页不满足对齐，切下来归还 PageCache
}

size_t trailing = span->_n - k;
if (trailing > 0) {
    // 对齐 Span 后面多出来的页，也切下来归还 PageCache
}

return span;
```

它的思路和 gperftools release 版一致：**多申请、找对齐位置、切掉前后多余页**。

示意图：

```text
NewSpan(k + alignPages) 得到一整段页：

raw span:
+----------+--------------------+----------+
| leading  | aligned user span  | trailing |
+----------+--------------------+----------+
           ^
           |
        alignedPageID，满足 alignment
```

然后：

1. `leading` 部分创建成一个空闲 `Span`，调用 `ReleaseSpanToPageCache` 归还。
2. 中间的 `aligned user span` 保留为本次分配结果。
3. `trailing` 部分也创建成空闲 `Span`，归还 PageCache。
4. 对最终返回的 span 设置为 `SpanState::Direct`，并为它覆盖页号到 span 的映射。

最终返回：

```cpp
return (void *)(span->_pageID << PAGE_SHIFT);
```

也就是说，用户拿到的指针就是 Span 起点，而不是某个原始块内部的偏移地址。

释放时：

```text
free(p)
    -> tcfree(p)
    -> PageCache::MapObjectToSpan(p)
    -> span->_state == SpanState::Direct
    -> PageCache::ReleaseSpanToPageCache(span)
```

这里用 `SpanState::Direct` 区分页级分配和小对象分配：

- 小对象 Span 被切成很多块，状态为 `SpanState::Small`。
- 页级分配没有切小块，整个 Span 直接给用户，状态为 `SpanState::Direct`。

因此大 alignment 分配释放时不进入 `ThreadCache::Deallocate`，而是直接把整个 Span 归还给 PageCache。

这就是为什么本实现不再需要 header：无论 alignment 多大，返回给用户的地址都被设计成“可以直接释放的块起点”。

## 主要代码改动

- `ConcurrentAlloc.h` 只保留 `tcmalloc/tcfree` 声明。
- `ConcurrentAlloc.cpp` 提供专用 API 实现。
- `MallocReplacement.cpp` 导出标准 C/C++ 分配符号。
- `PageCache` 新增 `NewAligned`，负责页级大对齐分配。
- `CentralCache` 和 `PageCache` 单例改为函数局部静态，避免全局替换 `new/malloc` 后的初始化顺序问题。
- `SpanList` 哨兵节点通过 `SystemAlloc + placement new` 创建，避免内部元数据走被替换的全局 `new`。

## 构建与验证

构建：

```bash
cmake -S . -B build
cmake --build build
```

直接链接验证：

```bash
./build/standard_alloc_test
```

`LD_PRELOAD` 验证：

```bash
LD_PRELOAD=/home/ubuntu/memPool/build/libmempool.so ./build/standard_alloc_plain
```

benchmark 对比：

```bash
./build/mempool_benchmark
./build/system_benchmark
./build/gperftools_benchmark
```

`benchmark.cpp` 现在不再包含 `ConcurrentAlloc.h`，也不直接调用 `tcmalloc/tcfree`。它使用更贴近日常 C++ 业务代码的分配方式：

- `std::make_unique`
- `std::vector<std::unique_ptr<T>>`
- `std::vector<std::string>`
- 多线程 `unique_ptr` 对象生命周期，线程数使用 `std::thread::hardware_concurrency()` 按当前机器核数决定
- `alignas(...)` 对齐对象的 `make_unique`

CMake 会用同一份源码生成三个可执行文件：

- `mempool_benchmark`：链接 `libmempool.so`，标准分配接口会进入本项目内存池。
- `system_benchmark`：不链接 `libmempool.so`，标准分配接口走系统默认分配器。
- `gperftools_benchmark`：链接系统安装的 `libtcmalloc.so`，标准分配接口走 gperftools tcmalloc。

这样可以比较“只改变链接方式”带来的性能差异，更接近真实接入方式。

本机确认 `gperftools_benchmark` 链接到了系统 gperftools：

```bash
ldd build/gperftools_benchmark | rg 'tcmalloc'
```

输出包含：

```text
libtcmalloc.so.4 => /lib/x86_64-linux-gnu/libtcmalloc.so.4
```

一次三轮连续测试结果如下。当前机器检测到 2 个硬件线程，多线程测试使用 2 个线程。单位为 ms，数值越低表示越快：

| 测试场景 | 本项目 `mempool_benchmark` | 系统默认 `system_benchmark` | gperftools `gperftools_benchmark` |
| --- | ---: | ---: | ---: |
| make_unique small objects | 3.662 - 5.388 | 3.908 - 5.556 | 2.972 - 3.784 |
| make_unique mixed objects | 3.197 - 3.477 | 3.169 - 5.151 | 2.578 - 3.956 |
| vector<string> workload | 8.329 - 9.492 | 9.313 - 16.349 | 7.760 - 11.237 |
| multi-thread unique_ptr | 2.444 - 8.639 | 3.172 - 4.478 | 2.161 - 2.509 |
| aligned make_unique | 2.835 - 4.339 | 7.563 - 10.496 | 2.721 - 3.838 |

从这轮结果看：

- gperftools 在小对象、混合对象和多线程 unique_ptr 场景下整体表现最好。
- 本项目内存池在字符串和 aligned make_unique 场景下明显快于系统默认分配器。
- 本项目多线程 unique_ptr 场景波动较大，最好值接近 gperftools，但最差值受调度影响明显。
- benchmark 数值会受系统负载影响，建议关注多轮区间而不是单次结果。

## 当前边界

- 当前目标平台是 Linux/glibc。
- 当前桶布局要求 `alignof(std::max_align_t) == 16`。
- 未实现 gperftools 的 hook、profiler、sampling、emergency malloc 等扩展能力。
- 本次重点是标准分配接口替换和 gperftools 风格 alignment 策略。
