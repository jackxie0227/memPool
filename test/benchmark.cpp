#include <algorithm>
#include <array>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono;

namespace
{
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

    struct SmallObject
    {
        int id;
        double score;
        std::array<char, 48> payload;

        explicit SmallObject(int value) : id(value), score(value * 0.25)
        {
            payload.fill(static_cast<char>(value));
        }
    };

    struct MediumObject
    {
        std::array<char, 512> payload;

        explicit MediumObject(size_t seed)
        {
            payload.fill(static_cast<char>(seed));
        }
    };

    struct alignas(64) CacheLineObject
    {
        std::array<char, 64> payload;

        explicit CacheLineObject(size_t seed)
        {
            payload.fill(static_cast<char>(seed));
        }
    };

    struct alignas(8192) PageAlignedObject
    {
        std::array<char, 8192> payload;

        explicit PageAlignedObject(size_t seed)
        {
            payload.fill(static_cast<char>(seed));
        }
    };

    void PrintResult(const char *name, double ms)
    {
        std::cout << std::left << std::setw(32) << name << ": "
                  << std::fixed << std::setprecision(3) << ms << " ms" << std::endl;
    }

    void Warmup()
    {
        std::vector<std::unique_ptr<SmallObject>> objects;
        objects.reserve(5000);
        for (int i = 0; i < 5000; ++i)
        {
            objects.emplace_back(std::make_unique<SmallObject>(i));
        }
    }

    double MakeUniqueSmallObjects()
    {
        constexpr size_t NUM_ALLOCS = 120000;
        Timer t;
        std::vector<std::unique_ptr<SmallObject>> objects;
        objects.reserve(NUM_ALLOCS);

        for (size_t i = 0; i < NUM_ALLOCS; ++i)
        {
            objects.emplace_back(std::make_unique<SmallObject>(static_cast<int>(i)));
            if (i % 4 == 0)
            {
                objects.pop_back();
            }
        }
        return t.elapsed();
    }

    double MakeUniqueMixedObjects()
    {
        constexpr size_t NUM_ALLOCS = 60000;
        Timer t;
        std::vector<std::unique_ptr<SmallObject>> smallObjects;
        std::vector<std::unique_ptr<MediumObject>> mediumObjects;
        smallObjects.reserve(NUM_ALLOCS);
        mediumObjects.reserve(NUM_ALLOCS / 4);

        for (size_t i = 0; i < NUM_ALLOCS; ++i)
        {
            smallObjects.emplace_back(std::make_unique<SmallObject>(static_cast<int>(i)));
            if (i % 4 == 0)
            {
                mediumObjects.emplace_back(std::make_unique<MediumObject>(i));
            }
            if (i % 128 == 0 && !smallObjects.empty())
            {
                size_t releaseCount = std::min(smallObjects.size(), size_t(32));
                smallObjects.erase(smallObjects.end() - releaseCount, smallObjects.end());
            }
        }
        return t.elapsed();
    }

    double VectorStringWorkload()
    {
        constexpr size_t NUM_ITEMS = 40000;
        Timer t;
        std::vector<std::string> strings;
        strings.reserve(NUM_ITEMS);

        for (size_t i = 0; i < NUM_ITEMS; ++i)
        {
            strings.emplace_back(24 + (i % 220), static_cast<char>('a' + (i % 26)));
            if (i % 64 == 0)
            {
                strings.emplace_back(std::to_string(i) + "-allocation-heavy-string-payload");
            }
            if (i % 256 == 0 && strings.size() > 128)
            {
                strings.erase(strings.begin(), strings.begin() + 64);
            }
        }

        size_t total = 0;
        for (const auto &s : strings)
        {
            total += s.size();
        }
        // 防止优化器把字符串工作负载整体消掉。
        if (total == 0)
        {
            std::cerr << "unexpected empty workload\n";
        }
        return t.elapsed();
    }

    double MultiThreadedUniquePtr()
    {
        const size_t numThreads = std::max(1u, std::thread::hardware_concurrency());
        constexpr size_t ALLOCS_PER_THREAD = 30000;

        auto threadFunc = [](size_t threadId)
        {
            std::mt19937 gen(static_cast<uint32_t>(0xBADC0DE + threadId));
            std::uniform_int_distribution<int> percentDist(0, 99);
            std::vector<std::unique_ptr<SmallObject>> objects;
            objects.reserve(ALLOCS_PER_THREAD);

            for (size_t i = 0; i < ALLOCS_PER_THREAD; ++i)
            {
                objects.emplace_back(std::make_unique<SmallObject>(static_cast<int>(i + threadId)));
                if (percentDist(gen) < 75 && !objects.empty())
                {
                    size_t index = static_cast<size_t>(gen()) % objects.size();
                    objects[index] = std::move(objects.back());
                    objects.pop_back();
                }
            }
        };

        Timer t;
        std::vector<std::thread> threads;
        for (size_t i = 0; i < numThreads; ++i)
        {
            threads.emplace_back(threadFunc, i);
        }
        for (auto &thread : threads)
        {
            thread.join();
        }
        return t.elapsed();
    }

    double AlignedMakeUnique()
    {
        constexpr size_t NUM_ALLOCS = 25000;
        Timer t;
        std::vector<std::unique_ptr<CacheLineObject>> cacheLineObjects;
        std::vector<std::unique_ptr<PageAlignedObject>> pageAlignedObjects;
        cacheLineObjects.reserve(NUM_ALLOCS);
        pageAlignedObjects.reserve(NUM_ALLOCS / 16);

        for (size_t i = 0; i < NUM_ALLOCS; ++i)
        {
            cacheLineObjects.emplace_back(std::make_unique<CacheLineObject>(i));
            if (i % 16 == 0)
            {
                pageAlignedObjects.emplace_back(std::make_unique<PageAlignedObject>(i));
            }
            if (i % 64 == 0 && !cacheLineObjects.empty())
            {
                cacheLineObjects.pop_back();
            }
            if (i % 128 == 0 && !pageAlignedObjects.empty())
            {
                pageAlignedObjects.pop_back();
            }
        }
        return t.elapsed();
    }
}

int main()
{
    std::cout << "Benchmark mode: modern C++ allocation APIs" << std::endl;
    std::cout << "Detected hardware threads: "
              << std::max(1u, std::thread::hardware_concurrency()) << std::endl;
    Warmup();

    PrintResult("make_unique small objects", MakeUniqueSmallObjects());
    PrintResult("make_unique mixed objects", MakeUniqueMixedObjects());
    PrintResult("vector<string> workload", VectorStringWorkload());
    PrintResult("multi-thread unique_ptr", MultiThreadedUniquePtr());
    PrintResult("aligned make_unique", AlignedMakeUnique());
    return 0;
}