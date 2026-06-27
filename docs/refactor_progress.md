# memPool 重构进度

## 阶段 0 - 构建与测试基线

日期：2026-06-27

### 当前分支与工作区

- 分支：`master...origin/master`。
- 建立基线时，工作区中已经存在本阶段范围外的未提交修改：
  `ConcurrentAlloc.cpp`、`MallocReplacement.cpp`、`PageCache.cpp`、`common.h`、
  已删除的 `change_docs/LINKED_ALLOCATOR_CHANGES.md`，以及未跟踪的 `.vscode/`、
  `AGENTS.md`、`docs/`。
- 阶段 0 的修改仅限于 CMake/CTest 接入和本文档。
  本阶段没有刻意修改 allocator 的 allocation/free/Span/PageMap/CentralCache/
  ThreadCache 行为。

### 当前代码结构

- `common.h`：共享常量、size-class 辅助函数、`SystemAlloc`/`SystemFree`、
  `FreeList`、`Span`、`SpanList`。
- `ObjectPool.h`：allocator 内部对象池，底层使用 `SystemAlloc`。
- `PageMap.h`：页号到 `Span*` 的 radix-map 变体。
- `PageCache.h/.cpp`：进程级 page-span cache、page map 归属、span 切分/合并、
  大对象和页对齐 span 路径。
- `CentralCache.h/.cpp`：进程级、按 size-class 分桶的 SpanList，在
  `ThreadCache` 和 `PageCache` 之间搬运对象区间。
- `ThreadCache.h/.cpp`：线程本地 freelist，以及与 `CentralCache` 之间的
  slow-start 批量搬运。
- `ConcurrentAlloc.h/.cpp`：导出的 `tcmalloc`/`tcfree` 入口，以及线程本地
  cache 获取逻辑。
- `MallocReplacement.cpp`：C/C++ 分配替换 API 表面。
- `benchmark.cpp`：mempool、系统 allocator、可选 gperftools 构建共用的
  基准测试负载。
- `standard_alloc_test.cpp`：标准分配替换的冒烟/回归测试。

### 当前公开 API

- 项目内部 API：
  - `void* tcmalloc(size_t size)`
  - `void tcfree(void* ptr)`
- C 分配替换导出：
  - `malloc`
  - `free`
  - `calloc`
  - `realloc`
  - `aligned_alloc`
  - `posix_memalign`
- C++ 分配替换导出：
  - 标量 `operator new` / `operator delete`
  - 数组 `operator new[]` / `operator delete[]`
  - nothrow new/delete 变体
  - sized delete 变体
  - C++17 对齐 new/delete 变体，包括数组、nothrow 和 sized 形式

### 基线构建命令

本机普通构建；当前机器可找到 `libtcmalloc.so`：

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

通过忽略本地/系统 tcmalloc 库路径，模拟无 gperftools 的构建：

```sh
cmake -S . -B build-no-tcmalloc-sim2 \
  -DCMAKE_IGNORE_PREFIX_PATH=/home/ubuntu/gperftools\;/usr\;/lib \
  -DCMAKE_IGNORE_PATH=/home/ubuntu/gperftools/lib\;/home/ubuntu/gperftools/.libs\;/usr/lib/x86_64-linux-gnu\;/lib/x86_64-linux-gnu
cmake --build build-no-tcmalloc-sim2 --target mempool standard_alloc_test
ctest --test-dir build-no-tcmalloc-sim2 --output-on-failure
```

### 当前测试结果

- `cmake -S . -B build`：通过。CMake 检测到
  `/home/ubuntu/gperftools/lib/libtcmalloc.so`，并启用
  `gperftools_benchmark`。
- `cmake --build build`：通过。已构建 `mempool`、`mempool_benchmark`、
  `system_benchmark`、`gperftools_benchmark`、`standard_alloc_test` 和
  `standard_alloc_plain`。
- `ctest --test-dir build --output-on-failure`：通过，
  `1/1 standard_alloc_test`。
- 模拟无 gperftools 的配置：通过。CMake 输出
  `tcmalloc library not found; skipping gperftools_benchmark`。
- 模拟无 gperftools 的核心目标构建：`mempool` 和 `standard_alloc_test`
  通过。
- 模拟无 gperftools 的 CTest：通过，`1/1 standard_alloc_test`。

### 阶段 0 完成内容

- 启用 `enable_testing()`。
- 将 `standard_alloc_test` 注册为 CTest 测试。
- 通过 `find_library(TCMALLOC_LIBRARY)` 将 `gperftools_benchmark` 改为可选。
- 为找到/跳过 gperftools 基准测试增加明确的 CMake 状态信息。
- 保持基准测试目标与核心测试路径相互独立。

### 本阶段未修复的已知架构风险

- 多个 allocator 路径仍使用手写 `lock()`/`unlock()`，尚未改为作用域锁；
  其中包括 CentralCache/PageCache 交互路径。
- `ThreadCache` 生命周期是线程本地的，但当前基线没有可见的清理路径。
- `PageCache::MapObjectToSpan` 在映射缺失时使用 assert；非法 free 未按
  hardened/debug allocator 处理。对当前 release 对齐目标这是可接受边界，
  但误用时仍有崩溃风险。
- 当前 `PageMap` 写入会通过 `ObjectPool` 分配内部 radix 节点；这点必须继续
  保持，以避免递归进入导出的 malloc/new。
- 大对象和对齐分配路径依赖 split/coalesce 过程中的 page-map 正确性。
- 现有测试仍是冒烟级别，尚未覆盖高并发、cache 回收、span 合并或
  大量非法输入边界。

### 未完成项

- 尚未执行 allocator 生命周期或锁协议重构。
- 本阶段没有新增 allocator 语义测试。
- 基准测试仅作为构建目标，刻意没有注册为阻塞 CTest 测试。

## 阶段 2 - 引入 SpanState，修正 Span 类型判断

日期：2026-06-27

### 修改文件

- `common.h`
- `PageCache.h`
- `PageCache.cpp`
- `CentralCache.cpp`
- `ConcurrentAlloc.cpp`
- `MallocReplacement.cpp`
- `CMakeLists.txt`
- `span_state_test.cpp`
- `docs/LINKED_ALLOCATOR_CHANGES.md`
- `docs/refactor_progress.md`

### 完成内容

- 为 `Span` 增加 `SpanState`，当前三种状态为：
  - `Free`：位于 PageCache 空闲 Span 链表，可被切分或合并。
  - `Small`：已经切成固定大小 slot，由 CentralCache 管理。
  - `Direct`：整页级分配，直接交给用户使用。
- 删除旧的布尔使用标记字段及全部读写，PageCache 合并相邻 Span 时改为只合并
  `_state == SpanState::Free` 的邻居。
- 将 `PageCache::NewSpan()` 调整为
  `NewSpan(size_t k, SpanState targetState)`；调用方必须传入
  `SpanState::Small` 或 `SpanState::Direct`，避免普通活跃 Span 离开
  Free Span 链表后仍保持 Free 状态。
- 增加 `Span::Reset(PageID, size_t, SpanState)`，只用于新建或已摘链的
  Span metadata 重新赋予页范围；`ReleaseSpanToPageCache()` 合并过程中不
  调用会清空 `_pageID` 或 `_n` 的通用 Reset。
- 固定 `use_count` 语义：它只在 `SpanState::Small` 时表示已经离开
  CentralCache freelist 的 slot 数，这些 slot 可能在 ThreadCache 中或被
  用户持有。`use_count` 不是 Span 类型标记；计数为零不能表示 Direct 或
  Free。
- `tcfree()` 改为按 `_state` 分类：
  `Small` 进入 ThreadCache，`Direct` 归还 PageCache，`Free` 或未知状态
  fail-fast。
- `UsableSizeForPointer()` 改为通过 `SpanUsableSize()` 按 `_state` 返回
  Small slot 大小或 Direct 页容量，不再用 `use_count` 判断容量分支。
- `CentralCache::ReleaseListToSpans()` 保留 Small Span 外借 slot 计数归零判断，
  但该判断只用于 Small Span 外借 slot 计数归零后的既有回收流程；判断前已
  断言 `span->_state == SpanState::Small` 和 `span->use_count > 0`。
- `NewAligned()` 通过 `NewSpan(k + alignPages, SpanState::Direct)` 获得的
  oversized Span 仍作为本阶段允许的内部临时 Direct Span；最终返回 Span
  保持 Direct，leading/trailing Span 以 Free 状态归还 PageCache。本阶段
  不修改其 PageMap 生命周期。

### 新增测试

- 新增 `span_state_test` CTest 目标，覆盖：
  - `Span::Reset(..., SpanState::Free)` 后 Free Span 元数据初始化。
  - Small 分配后对应 Span 为 `SpanState::Small`，`_objSize` 正确。
  - 白盒验证 `state == Small` 且 `use_count` 计数为零时，
    `SpanUsableSize()` 仍按 Small 返回 `_objSize`，不会解释为 Direct。
  - 大对象分配后对应 Span 为 `SpanState::Direct`，`use_count` 为零且
    `_freeList == nullptr`。
  - Small 和 Direct 指针的 `realloc` 基本容量分支仍可运行。

### 验证结果

- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug`：通过。
- `cmake --build build -j`：通过。
- `ctest --test-dir build --output-on-failure`：通过，
  `2/2` 测试通过（`standard_alloc_test`、`span_state_test`）。
- `rg -n "use_count\\s*==\\s*0|use_count\\s*!=\\s*0" .`：仅命中
  `CentralCache.cpp` 中 Small Span 外借 slot 计数归零判断。
- 旧的布尔使用标记字段搜索：无命中。
- `rg -n "SpanState::Free|SpanState::Small|SpanState::Direct" .`：覆盖
  Free/Small/Direct 初始化、`tcfree()` 分类和 usable-size 分类。

### 尚未实现和已知限制

- 未重构 PageMap 生命周期；`NewAligned()` oversized Direct Span 的旧映射
  清理留到阶段 3。
- 未实现 Direct 起始地址校验、Direct double free 防护、Small slot 边界
  检查或跨线程 double free 检测。
- 未实现 Small Span CentralCache 完整回收重构、`_slotCount` 和 slot 统计。
- 未实现 ThreadCache Flush、完整 realloc 合法性验证、OOM 策略重构。
- 未实现 hardened/debug allocator 能力，包括 allocation bitmap、redzone、
  canary、use-after-free 填充和 new/delete 类型匹配检测。
