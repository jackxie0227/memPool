#include <algorithm>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <random>
#include <regex>
#include <sstream>
#include <string>
#include <thread>

namespace
{
    constexpr double kTrialNSec = 0.3e9;

    double benchmarkMinTimeNSec = 3.0e9;
    int benchmarkRepetitions = 3;
    bool benchmarkListOnly = false;
    bool benchmarkSkipRandomize = false;
    std::unique_ptr<std::regex> benchmarkFilter;

    using BenchBody = void (*)(long iterations, uintptr_t param);

    struct Benchmark
    {
        BenchBody body;
        uintptr_t param;
    };

    size_t PointerSlotCapacity(size_t usedSlots)
    {
        constexpr size_t kAllocatorBoundaryBytes = 256 * 1024;
        if (usedSlots * sizeof(void *) == kAllocatorBoundaryBytes)
        {
            return usedSlots + 1;
        }
        return usedSlots;
    }

    void PrintUsage(const char *program)
    {
        std::cout << program << " [benchmark flags]\n"
                  << "  --help\n"
                  << "  --benchmark_filter=<regex>\n"
                  << "  --benchmark_list\n"
                  << "  --benchmark_min_time=<seconds>\n"
                  << "  --benchmark_repetitions=<count>\n"
                  << "  --benchmark_skip_randomize\n";
    }

    bool HasPrefix(const std::string &value, const char *prefix)
    {
        const size_t prefixLen = std::strlen(prefix);
        return value.size() >= prefixLen && value.compare(0, prefixLen, prefix) == 0;
    }

    void InitBenchmark(int argc, char **argv)
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg(argv[i]);
            if (arg == "--help")
            {
                PrintUsage(argv[0]);
                std::exit(0);
            }
            else if (arg == "--benchmark_list")
            {
                benchmarkListOnly = true;
            }
            else if (arg == "--benchmark_skip_randomize")
            {
                benchmarkSkipRandomize = true;
            }
            else if (HasPrefix(arg, "--benchmark_min_time="))
            {
                const char *value = arg.c_str() + std::strlen("--benchmark_min_time=");
                char *end = nullptr;
                const double seconds = std::strtod(value, &end);
                if (end == value || *end != '\0' || seconds <= 0.0)
                {
                    std::cerr << "failed to parse benchmark_min_time argument: " << arg << '\n';
                    std::exit(1);
                }
                benchmarkMinTimeNSec = seconds * 1e9;
            }
            else if (HasPrefix(arg, "--benchmark_repetitions="))
            {
                const char *value = arg.c_str() + std::strlen("--benchmark_repetitions=");
                char *end = nullptr;
                const long repetitions = std::strtol(value, &end, 0);
                if (end == value || *end != '\0' || repetitions < 1 || repetitions > INT_MAX)
                {
                    std::cerr << "failed to parse benchmark_repetitions argument: " << arg << '\n';
                    std::exit(1);
                }
                benchmarkRepetitions = static_cast<int>(repetitions);
            }
            else if (HasPrefix(arg, "--benchmark_filter="))
            {
                const char *pattern = arg.c_str() + std::strlen("--benchmark_filter=");
                try
                {
                    benchmarkFilter = std::make_unique<std::regex>(pattern);
                }
                catch (const std::regex_error &e)
                {
                    std::cerr << "failed to parse benchmark_filter argument: " << e.what() << '\n';
                    std::exit(1);
                }
            }
            else
            {
                std::cerr << "unknown benchmark flag: " << arg << '\n';
                PrintUsage(argv[0]);
                std::exit(1);
            }
        }
    }

    double MeasureOnce(const Benchmark &benchmark, long iterations)
    {
        const auto before = std::chrono::steady_clock::now();
        benchmark.body(iterations, benchmark.param);
        const auto after = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::nano>(after - before).count();
    }

    double RunBenchmark(const Benchmark &benchmark)
    {
        long iterations = 128;
        double nsec = 0.0;

        do
        {
            nsec = MeasureOnce(benchmark, iterations);
            if (nsec > kTrialNSec)
            {
                break;
            }
            if (iterations > LONG_MAX / 2)
            {
                std::cerr << "benchmark iteration count overflow\n";
                std::exit(1);
            }
            iterations <<= 1;
        } while (true);

        while (nsec < benchmarkMinTimeNSec)
        {
            const double targetIterations = iterations * benchmarkMinTimeNSec * 1.1 / nsec;
            if (targetIterations > static_cast<double>(LONG_MAX))
            {
                std::cerr << "benchmark target iteration count overflow\n";
                std::exit(1);
            }
            iterations = static_cast<long>(targetIterations);
            if (iterations < 1)
            {
                iterations = 1;
            }
            nsec = MeasureOnce(benchmark, iterations);
        }

        return nsec / iterations;
    }

    std::string FullBenchmarkName(const char *name, uintptr_t param)
    {
        std::ostringstream fullName;
        fullName << name;
        if (param != 0)
        {
            fullName << '(' << param << ')';
        }
        return fullName.str();
    }

    void ReportBenchmark(const char *name, BenchBody body, uintptr_t param)
    {
        const std::string fullName = FullBenchmarkName(name, param);

        if (benchmarkListOnly)
        {
            std::cout << "known benchmark: " << fullName << '\n';
            return;
        }

        if (benchmarkFilter && !std::regex_search(fullName, *benchmarkFilter))
        {
            return;
        }

        const Benchmark benchmark{body, param};
        for (int i = 0; i < benchmarkRepetitions; ++i)
        {
            std::cout << "Benchmark: " << std::left << std::setw(48) << fullName << std::right << std::flush;
            const double nsec = RunBenchmark(benchmark);
            std::cout << std::fixed << std::setprecision(3)
                      << nsec << " nsec (rate: " << (1e9 / nsec / 1e6) << " Mops/sec)\n";
        }
    }

    void BenchFastpathThroughput(long iterations, uintptr_t)
    {
        size_t size = 32;
        for (; iterations > 0; --iterations)
        {
            void *ptr = operator new(size);
            operator delete(ptr);
            size = ((size * 8191) & 511) + 16;
        }
    }

    void BenchFastpathDependent(long iterations, uintptr_t)
    {
        size_t size = 32;
        for (; iterations > 0; --iterations)
        {
            const uintptr_t ptr = reinterpret_cast<uintptr_t>(operator new(size));
            operator delete(reinterpret_cast<void *>(ptr));
            size = ((size | static_cast<size_t>(ptr)) & 511) + 16;
        }
    }

    void BenchFastpathSimple(long iterations, uintptr_t param)
    {
        const size_t size = static_cast<size_t>(param);
        for (; iterations > 0; --iterations)
        {
            void *ptr = operator new(size);
            operator delete(ptr);
        }
    }

#if defined(__cpp_sized_deallocation)
    void BenchFastpathSimpleSized(long iterations, uintptr_t param)
    {
        const size_t size = static_cast<size_t>(param);
        for (; iterations > 0; --iterations)
        {
            void *ptr = operator new(size);
            operator delete(ptr, size);
        }
    }
#endif

#if defined(__cpp_aligned_new)
    void BenchFastpathMemalign(long iterations, uintptr_t param)
    {
        const size_t size = static_cast<size_t>(param);
        constexpr std::align_val_t kAlign{32};
        for (; iterations > 0; --iterations)
        {
            void *ptr = operator new(size, kAlign);
            operator delete(ptr, size, kAlign);
        }
    }
#endif

    void BenchFastpathStack(long iterations, uintptr_t param)
    {
        size_t size = 64;
        const long depth = std::max(1L, static_cast<long>(param));
        auto stack = std::make_unique<void *[]>(PointerSlotCapacity(static_cast<size_t>(depth)));

        for (; iterations > 0; iterations -= depth)
        {
            for (long k = depth - 1; k >= 0; --k)
            {
                void *ptr = operator new(size);
                stack[static_cast<size_t>(k)] = ptr;
                size = ((size | reinterpret_cast<size_t>(ptr)) & 511) + 16;
            }
            for (long k = 0; k < depth; ++k)
            {
                operator delete(stack[static_cast<size_t>(k)]);
            }
        }
    }

    void BenchFastpathStackSimple(long iterations, uintptr_t param)
    {
        constexpr size_t kSize = 32;
        const long depth = std::max(1L, static_cast<long>(param));
        auto stack = std::make_unique<void *[]>(PointerSlotCapacity(static_cast<size_t>(depth)));

        for (; iterations > 0; iterations -= depth)
        {
            for (long k = depth - 1; k >= 0; --k)
            {
                stack[static_cast<size_t>(k)] = operator new(kSize);
            }
            for (long k = 0; k < depth; ++k)
            {
#if defined(__cpp_sized_deallocation)
                operator delete(stack[static_cast<size_t>(k)], kSize);
#else
                operator delete(stack[static_cast<size_t>(k)]);
#endif
            }
        }
    }

    void BenchFastpathRndDependent(long iterations, uintptr_t param)
    {
        constexpr uintptr_t kRndC = 1013904223;
        constexpr uintptr_t kRndA = 1664525;

        if ((param & (param - 1)) != 0)
        {
            std::cerr << "bench_fastpath_rnd_dependent param must be a power of two\n";
            std::exit(1);
        }

        size_t size = 128;
        const long count = std::max(1L, static_cast<long>(param));
        auto ptrs = std::make_unique<void *[]>(PointerSlotCapacity(static_cast<size_t>(count)));

        for (; iterations > 0; iterations -= count)
        {
            for (long k = count - 1; k >= 0; --k)
            {
                void *ptr = operator new(size);
                ptrs[static_cast<size_t>(k)] = ptr;
                size = ((size | reinterpret_cast<size_t>(ptr)) & 511) + 16;
            }

            uint32_t rnd = 0;
            uint32_t freeIndex = 0;
            do
            {
                operator delete(ptrs[freeIndex]);
                rnd = rnd * kRndA + kRndC;
                freeIndex = rnd & (static_cast<uint32_t>(count) - 1);
            } while (freeIndex != 0);
        }
    }

    void BenchFastpathRndDependent8Threads(long iterations, uintptr_t param)
    {
        auto body = [iterations, param]()
        {
            BenchFastpathRndDependent(iterations, param);
        };

        std::thread threads[] = {
            std::thread(body), std::thread(body), std::thread(body), std::thread(body),
            std::thread(body), std::thread(body), std::thread(body), std::thread(body)};

        for (auto &thread : threads)
        {
            thread.join();
        }
    }

    void RandomizeOneSizeClass(size_t size)
    {
        constexpr size_t kBytesPerSizeClass = 4 << 20;
        constexpr size_t kMinObjects = 64;
        constexpr size_t kMaxObjects = 65536;

        size_t count = kBytesPerSizeClass / size;
        count = std::max(kMinObjects, std::min(count, kMaxObjects));

        auto objects = std::make_unique<void *[]>(count);
        for (size_t i = 0; i < count; ++i)
        {
            objects[i] = operator new(size);
        }

        std::minstd_rand random(static_cast<unsigned int>(size * 2654435761u));
        std::shuffle(objects.get(), objects.get() + count, random);

        for (size_t i = 0; i < count; ++i)
        {
            operator delete(objects[i]);
        }
    }

    void RandomizeSizeClasses()
    {
        RandomizeOneSizeClass(8);

        int size = 16;
        for (; size < 256; size += 16)
        {
            RandomizeOneSizeClass(static_cast<size_t>(size));
        }
        for (; size < 512; size += 32)
        {
            RandomizeOneSizeClass(static_cast<size_t>(size));
        }
        for (; size < 1024; size += 64)
        {
            RandomizeOneSizeClass(static_cast<size_t>(size));
        }
        for (; size < (4 << 10); size += 128)
        {
            RandomizeOneSizeClass(static_cast<size_t>(size));
        }
        for (; size < (32 << 10); size += 1024)
        {
            RandomizeOneSizeClass(static_cast<size_t>(size));
        }
    }

    void RegisterBenchmarks()
    {
        ReportBenchmark("bench_fastpath_throughput", BenchFastpathThroughput, 0);
        ReportBenchmark("bench_fastpath_dependent", BenchFastpathDependent, 0);
        ReportBenchmark("bench_fastpath_simple", BenchFastpathSimple, 64);
        ReportBenchmark("bench_fastpath_simple", BenchFastpathSimple, 2048);
        ReportBenchmark("bench_fastpath_simple", BenchFastpathSimple, 16384);

#if defined(__cpp_sized_deallocation)
        ReportBenchmark("bench_fastpath_simple_sized", BenchFastpathSimpleSized, 64);
        ReportBenchmark("bench_fastpath_simple_sized", BenchFastpathSimpleSized, 2048);
#endif

#if defined(__cpp_aligned_new)
        ReportBenchmark("bench_fastpath_memalign", BenchFastpathMemalign, 64);
        ReportBenchmark("bench_fastpath_memalign", BenchFastpathMemalign, 2048);
#endif

        for (uintptr_t depth = 8; depth <= 512; depth <<= 1)
        {
            ReportBenchmark("bench_fastpath_stack", BenchFastpathStack, depth);
        }

        ReportBenchmark("bench_fastpath_stack_simple", BenchFastpathStackSimple, 32);
        ReportBenchmark("bench_fastpath_stack_simple", BenchFastpathStackSimple, 8192);
        ReportBenchmark("bench_fastpath_stack_simple", BenchFastpathStackSimple, 32768);

        ReportBenchmark("bench_fastpath_rnd_dependent", BenchFastpathRndDependent, 32);
        ReportBenchmark("bench_fastpath_rnd_dependent", BenchFastpathRndDependent, 8192);
        ReportBenchmark("bench_fastpath_rnd_dependent", BenchFastpathRndDependent, 32768);

        ReportBenchmark("bench_fastpath_rnd_dependent_8threads", BenchFastpathRndDependent8Threads, 32768);
    }
}

int main(int argc, char **argv)
{
    InitBenchmark(argc, argv);

    if (!benchmarkListOnly && !benchmarkSkipRandomize)
    {
        std::cout << "Trying to randomize freelists..." << std::flush;
        RandomizeSizeClasses();
        std::cout << "done.\n";
    }

    RegisterBenchmarks();
    return 0;
}
