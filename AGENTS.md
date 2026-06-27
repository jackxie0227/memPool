# memPool incremental refactor rules

1. 本项目（系统路径 /home/ubuntu/memPool/））目标是对齐 gperftools 普通 release 版 TCMalloc 的核心生命周期和锁协议，
   不是实现 hardened/debug allocator，
   阅读 gperftools 源码时你需要在该路径中阅读：/home/ubuntu/gperftools
2. 每次只完成当前任务明确列出的范围；不得提前重构后续阶段。
3. 修改前先阅读相关源码、CMake 配置、已有测试，以及 docs/refactor_progress.md。
4. 不得执行 git commit、git reset、git rebase、git push、git clean 或改写历史。
   可以执行 git status、git diff、git log 进行只读检查。
5. allocator 内部元数据不得调用当前项目导出的 malloc/free/new/delete，
   不得递归进入 tcmalloc/tcfree、ThreadCache、CentralCache、PageCache 的用户对象路径。
6. Fatal 路径不得使用 std::string、iostream、std::format、stringstream 或任何可能分配内存的设施。
7. 不得新增跨多个分支的裸 lock()/unlock()；普通临界区使用 lock_guard，
   需要暂时释放锁时使用 unique_lock。
8. PageCache 不得在持有 PageCache 锁时获取 CentralCache 锁。
   CentralCache 需要访问 PageCache 时，必须遵循：
   Central 锁 -> 临时 unlock -> PageCache 公共线程安全接口 -> 重新 lock。
9. 每个任务结束后：
   - 编译并运行本阶段相关测试；
   - 输出修改文件、关键不变量、测试命令与结果；
   - 在 docs/refactor_progress.md 追加本阶段的完成内容、未完成项和已知限制。
10. 不实现以下 hardened/debug 能力：
    per-slot allocation bitmap、Small Object double free 可靠检测、
    跨线程 double free 检测、全局 allocation map、redzone、canary、
    use-after-free 填充、new/delete 类型匹配检测。
11. docs/refactor_progress.md 需要使用中文编写。