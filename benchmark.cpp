#include "ConcurrentAlloc.h"
#include <iostream>
#include <vector>
#include <chrono>
#include <random>
#include <iomanip>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cstring>

using namespace std::chrono;

// 简单计时器
class Timer
{
    high_resolution_clock::time_point start_;

public:
    Timer() : start_(high_resolution_clock::now()) {}
    double elapsed() const
    {
        auto end = high_resolution_clock::now();
        return duration_cast<microseconds>(end - start_).count() / 1000.0;
    }
};

class PerformanceTest
{
private:
    // 测试统计信息
    struct TestStats
    {
        double memPoolTime{0.0};
        double systemTime{0.0};
        size_t totalAllocs{0};
        size_t totalBytes{0};
    };

public:
    // 1. 系统预热
    static void warmup()
    {
        std::cout << "Warming up memory systems...\n";
        std::vector<void *> warmupPtrs;
        for (int i = 0; i < 1000; ++i)
        {
            for (size_t size : {32, 64, 128, 256, 512})
            {
                void *p = tcmalloc(size);
                warmupPtrs.emplace_back(p);
            }
        }
        for (const auto &ptr : warmupPtrs)
        {
            tcfree(ptr);
        }
        std::cout << "Warmup complete. \n\n";
    }

    // 2. 小对象分配测试
    static void testSmallAllocation()
    {
        // 设定参数 分配10万次 每次32字节
        constexpr size_t NUM_ALLOCS = 100000;
        constexpr size_t SMALL_SIZE = 32;
        std::cout << "\nTesting small allocations (" << NUM_ALLOCS << " allocations of "
                  << SMALL_SIZE << " bytes):" << std::endl;

        // 测试内存池
        {
            Timer t;
            std::vector<void *> ptrs;
            ptrs.reserve(NUM_ALLOCS);

            for (size_t i = 0; i < NUM_ALLOCS; ++i)
            {
                ptrs.push_back(tcmalloc(SMALL_SIZE));

                // 模拟真实使用：部分立即释放
                if (i % 4 == 0)
                {
                    tcfree(ptrs.back());
                    ptrs.pop_back();
                }
            }

            for (void *ptr : ptrs)
            {
                tcfree(ptr);
            }

            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3)
                      << t.elapsed() << " ms" << std::endl;
        }

        // 测试 new / delete
        {
            Timer t;
            std::vector<void *> ptrs;
            ptrs.reserve(NUM_ALLOCS);

            for (size_t i = 0; i < NUM_ALLOCS; ++i)
            {
                ptrs.push_back(new char[SMALL_SIZE]);
                if (i % 4 == 0)
                {
                    delete[] static_cast<char *>(ptrs.back());
                    ptrs.pop_back();
                }
            }
            for (void *ptr : ptrs)
            {
                delete[] static_cast<char *>(ptr);
            }

            std::cout << "New/Delete: " << std::fixed << std::setprecision(3)
                      << t.elapsed() << " ms" << std::endl;
        }
    }

    // 3. 多线程测试
    static void testMultiThreaded()
    {
        // 4 线程 每个线程分配2.5w次 分配大小范围上限
        constexpr size_t NUM_THREADS = 4;
        constexpr size_t ALLOCS_PER_THREAD = 25000;
        constexpr size_t MAX_SIZE = 256;

        std::cout << "\nTesting multi-threaded allocations (" << NUM_THREADS
                  << " threads, " << ALLOCS_PER_THREAD << " allocations each):"
                  << std::endl;

        auto threadFunc = [](bool useMemPool) // useMemPool - true 使用内存池
        {
            std::random_device rd;
            std::mt19937 gen(rd());
            std::uniform_int_distribution<> dis(8, MAX_SIZE);
            std::vector<void *> ptrs;
            ptrs.reserve(ALLOCS_PER_THREAD);

            for (size_t i = 0; i < ALLOCS_PER_THREAD; ++i)
            {
                size_t size = dis(gen);
                void *ptr = useMemPool ? tcmalloc(size)
                                       : new char[size];
                ptrs.push_back(ptr);

                // 随机释放一些内存
                if (rand() % 100 < 75)
                { // 75%的概率释放
                    size_t index = rand() % ptrs.size();
                    if (useMemPool)
                    {
                        tcfree(ptrs[index]);
                    }
                    else
                    {
                        delete[] static_cast<char *>(ptrs[index]);
                    }
                    ptrs[index] = ptrs.back();
                    ptrs.pop_back();
                }
            }

            // 清理剩余内存
            for (const auto &ptr : ptrs)
            {
                if (useMemPool)
                {
                    tcfree(ptr);
                }
                else
                {
                    delete[] static_cast<char *>(ptr);
                }
            }
        };

        // 测试内存池
        {
            Timer t;
            std::vector<std::thread> threads;

            for (size_t i = 0; i < NUM_THREADS; ++i)
            {
                threads.emplace_back(threadFunc, true);
            }

            for (auto &thread : threads)
            {
                thread.join();
            }

            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3)
                      << t.elapsed() << " ms" << std::endl;
        }

        // 测试 new/delete
        {
            Timer t;
            std::vector<std::thread> threads;

            for (size_t i = 0; i < NUM_THREADS; ++i)
            {
                threads.emplace_back(threadFunc, false);
            }

            for (auto &thread : threads)
            {
                thread.join();
            }

            std::cout << "New/Delete: " << std::fixed << std::setprecision(3)
                      << t.elapsed() << " ms" << std::endl;
        }
    }

    // 4. 混合大小测试
    static void testMixedSizes()
    {
        constexpr size_t NUM_ALLOCS = 50000;
        const size_t SIZES[] = {16, 32, 64, 128, 256, 512, 1024, 2048};

        std::cout << "\nTesting mixed size allocations (" << NUM_ALLOCS
                  << " allocations):" << std::endl;

        // 测试内存池
        {
            Timer t;
            std::vector<void *> ptrs;
            ptrs.reserve(NUM_ALLOCS);

            for (size_t i = 0; i < NUM_ALLOCS; ++i)
            {
                size_t size = SIZES[rand() % 8];
                void *p = tcmalloc(size);
                ptrs.emplace_back(p);

                // 批量释放
                if (i % 100 == 0 && !ptrs.empty())
                {
                    size_t releaseCount = std::min(ptrs.size(), size_t(20));
                    for (size_t j = 0; j < releaseCount; ++j)
                    {
                        tcfree(ptrs.back());
                        ptrs.pop_back();
                    }
                }
            }

            for (const auto ptr : ptrs)
            {
                tcfree(ptr);
            }

            std::cout << "Memory Pool: " << std::fixed << std::setprecision(3)
                      << t.elapsed() << " ms" << std::endl;
        }

        // 测试 new/delete
        {
            Timer t;
            std::vector<std::pair<void *, size_t>> ptrs;
            ptrs.reserve(NUM_ALLOCS);

            for (size_t i = 0; i < NUM_ALLOCS; ++i)
            {
                size_t size = SIZES[rand() % 8];
                void *p = new char[size];
                ptrs.emplace_back(p, size);

                if (i % 100 == 0 && !ptrs.empty())
                {
                    size_t releaseCount = std::min(ptrs.size(), size_t(20));
                    for (size_t j = 0; j < releaseCount; ++j)
                    {
                        delete[] static_cast<char *>(ptrs.back().first);
                        ptrs.pop_back();
                    }
                }
            }

            for (const auto &pair : ptrs)
            {
                delete[] static_cast<char *>(pair.first);
            }

            std::cout << "New/Delete: " << std::fixed << std::setprecision(3)
                      << t.elapsed() << " ms" << std::endl;
        }
    }
};

int main()
{
    PerformanceTest::warmup();
    PerformanceTest::testSmallAllocation();
    PerformanceTest::testMultiThreaded();
    PerformanceTest::testMixedSizes();
    return 0;
}